// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "coroutine.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

void Usage(void) {
  fprintf(stderr, "usage: client -j <jobs> <host> <filename>\n");
  exit(1);
}

// Send data to the server from a coroutine.
static bool SendToServer(co::Coroutine *c, int fd, const char *request,
                         size_t length) {
  int offset = 0;
  const size_t kMaxLength = 1024;
  while (length > 0) {
    // Wait until we can send to the network.  This will yield to other
    // coroutines and we will be resumed when we can write.
    c->Wait(fd, POLLOUT);
    size_t nbytes = length;
    if (nbytes > kMaxLength) {
      nbytes = kMaxLength;
    }
    ssize_t n = write(fd, request + offset, nbytes);
    if (n == -1) {
      perror("write");
      return false;
    }
    if (n == 0) {
      return false;
    }
    length -= n;
    offset += n;
  }
  return true;
}

static std::vector<std::string> SplitString(const std::string &str) {
  std::vector<std::string> result;
  std::istringstream iss(str);
  std::string token;

  while (std::getline(iss, token, ' ')) {
    result.push_back(token);
  }

  return result;
}

static size_t ReadHeaders(std::string &buffer, std::vector<std::string> &header,
                          std::map<std::string, std::string> &http_headers) {
  // Parse the header.
  size_t i = 0;
  // Find the \r\n at the end of the first line.
  while (i < buffer.size() && buffer[i] != '\r') {
    i++;
  }
  if (i == buffer.size()) {
    // No header line.
    return i;
  }
  std::string header_line(buffer.substr(0, i));
  i += 2; // Skip \r\n.

  // Parse the header line.  By splitting it at space into String objects
  // allocated from the heap.
  header = SplitString(header_line);

  // Extract MIME headers.  Holds pointers to the data in the buffer.  Does
  // not own the pointers.
  while (i < buffer.length()) {
    if (buffer[i] == '\r') {
      // End of headers is a blank line.
      i += 2;
      break;
    }
    char *name = &buffer[i];
    while (i < buffer.size() && buffer[i] != ':') {
      // Convert name to upper case as they are case insensitive.
      buffer[i] = toupper(buffer[i]);
      i++;
    }
    // No header value, end of headers.
    if (i == buffer.size()) {
      break;
    }
    buffer[i] = '\0'; // Replace : by EOL
    i++;
    // Skip to non-space.
    while (i < buffer.size() && isspace(buffer[i])) {
      i++;
    }
    char *value = &buffer[i];
    while (i < buffer.size()) {
      if (i < buffer.size() + 3 && buffer[i] == '\r') {
        // Check for continuation with a space as the first character on the
        // next line.  TAB too.
        if (buffer[i + 2] != ' ' && buffer[i + 2] != '\t') {
          break;
        }
      } else if (buffer[i] == '\r') {
        // No continuation, check for end of value.
        break;
      }
      i++;
    }
    buffer[i] = '\0'; // Replace \r by EOL
    i += 2;
    http_headers[name] = value;
  }
  return i;
}

static size_t ReadContents(co::Coroutine *c, int fd, std::string &buffer,
                           size_t i, int length, bool write_to_output) {
  while (length > 0) {
    if (i < buffer.size()) {
      // Data remaining in buffer
      size_t nbytes = buffer.size() - i;
      if (nbytes > length) {
        nbytes = length;
      }
      if (write_to_output) {
        fwrite(&buffer[i], 1, nbytes, stdout);
      }
      length -= nbytes;
      i += nbytes;
    } else {
      // No data in buffer, read some more into the buffer.
      buffer.clear();
      i = 0;
      char buf[256];
      c->Wait(fd, POLLIN);
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n == -1) {
        perror("read");
        break;
      }
      if (n == 0) {
        printf("done\n");
        break;
      }
      buffer += std::string(buf, n);
    }
  }
  return i;
}

static size_t ReadChunkLength(co::Coroutine *c, int fd, std::string &buffer,
                              size_t i, int *length) {
  for (;;) {
    char ch;
    if (i < buffer.size()) {
      ch = toupper(buffer[i++]);
    } else {
      // Fill the buffer with some more data.
      buffer.clear();
      i = 0;
      c->Wait(fd, POLLIN);
      char buf[256];
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n == -1) {
        perror("read");
        *length = 0;
        return i;
      }
      if (n == 0) {
        // Didn't read anything, EOF on input.
        return i;
      }
      buffer += std::string(buf, n);
      continue;
    }
    if (ch == '\r') {
      i++;
      break;
    }
    if (ch > '9') {
      ch = ch - 'A' + 10;
    } else {
      ch -= '0';
    }
    *length = (*length << 4) | ch;
  }
  return i;
}

static void ReadChunkedContents(co::Coroutine *c, int fd, std::string &buffer,
                                size_t i) {
  for (;;) {
    // First line is the length of the chunk in hex.
    int length = 0;
    i = ReadChunkLength(c, fd, buffer, i, &length);
    if (length == 0) {
      break;
    }
    i = ReadContents(c, fd, buffer, i, length, true);

    // Chunk is followed by a CRLF.  Don't print this, just skip it.
    i = ReadContents(c, fd, buffer, i, 2, false);
  }
}

void Client(co::Coroutine *c, std::string server_name, in_addr_t ipaddr,
            std::string filename) {

  int fd = socket(PF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    exit(1);
  }

  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(80),
                             .sin_len = sizeof(int),
                             .sin_addr.s_addr = ipaddr};
  int e = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (e != 0) {
    close(fd);
    perror("connect");
    return;
  }
  char request[256];

  int reqlen =
      snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n",
               filename.c_str(), server_name.c_str());
  bool ok = SendToServer(c, fd, request, reqlen);
  if (!ok) {
    fprintf(stderr, "Failed to send to server: %s\n", strerror(errno));
    close(fd);
    return;
  }

  std::string buffer;

  // Read incoming HTTP request and parse it.
  for (;;) {
    char buf[64];

    // Wait for data to arrive.  This will yield to other coroutines and
    // we will resume when data is available to read.
    c->Wait(fd, POLLIN);
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n == -1) {
      perror("read");
      close(fd);
      return;
    }
    if (n == 0) {
      // EOF while reading header, nothing we can do.
      close(fd);
      return;
    }
    // Append to data buffer.
    buffer += std::string(buf, n);

    // A blank line terminates the read.
    if (buffer.find("\r\n\r\n") != std::string::npos) {
      break;
    }
  }

  std::vector<std::string> header;
  std::map<std::string, std::string> http_headers;

  // The buffer contains the HTTP header line and the HTTP headers.
  size_t i = ReadHeaders(buffer, header, http_headers);

  const size_t kProtocol = 0;
  const size_t kStatus = 1;
  const size_t kError = 2;

  // Make alises for the http header fields.
  std::string &status = header[kStatus];
  std::string &protocol = header[kProtocol];

  // Check for valid status.
  int status_value = atoi(status.c_str());
  if (status_value != 200) {
    fprintf(stderr, "%s Error: %d: ", protocol.c_str(), status_value);
    // Print all error strings.
    const char *sep = "";
    for (size_t i = kError; i < header.size(); i++) {
      std::string &s = header[i];
      fprintf(stderr, "%s%s", sep, s.c_str());
      sep = " ";
    }
    fprintf(stderr, "\n");
  } else {
    // We are the end of the http headers in the buffer.  We now need to work
    // out the length.  This is either from the CONTENT-LENGTH header or if
    // TRANSFER-ENCODING is "chunked", we have a series of chunks, each of which
    // is preceded by a hex length on a line of its own and terminated with a
    // CRLF
    auto it = http_headers.find("TRANSFER-ENCODING");
    bool is_chunked = false;
    int content_length = -1;

    if (it != http_headers.end() && it->second == "chunked") {
      is_chunked = true;
    } else {
      auto it = http_headers.find("CONTENT-LENGTH");
      if (it != http_headers.end()) {
        content_length = (int)strtoll(it->second.c_str(), NULL, 10);
      }
    }

    // We use the buffer to hold all the data received, in blocks.
    if (is_chunked) {
      ReadChunkedContents(c, fd, buffer, i);
    } else {
      if (content_length == -1) {
        fprintf(stderr,
                "Don't know how many bytes to read, no Content-length in "
                "headers\n");
      } else {
        ReadContents(c, fd, buffer, i, content_length, true);
      }
    }
  }

  close(fd);
}

int main(int argc, const char *argv[]) {
  std::string host;
  std::string filename;
  int num_jobs = 1;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "-j") == 0) {
        // Allow -j N where N is a number
        i++;
        if (i < argc) {
          if (isdigit(argv[i][0])) {
            num_jobs = atoi(argv[i]);
          } else {
            Usage();
          }
        } else {
          Usage();
        }
      } else if (argv[i][1] == 'j' && isdigit(argv[i][2])) {
        // Allow -jN where N is a number.
        num_jobs = atoi(&argv[i][2]);
      } else {
        Usage();
      }
    } else {
      if (host.empty()) {
        host = argv[i];
      } else if (filename.empty()) {
        filename = argv[i];
      } else {
        Usage();
      }
    }
  }
  if (host.empty() || filename.empty()) {
    Usage();
  }

  struct hostent *entry = gethostbyname(host.c_str());
  if (entry == NULL) {
    fprintf(stderr, "unknown host %s\n", host.c_str());
    exit(1);
  }
  in_addr_t ipaddr = ((struct in_addr *)entry->h_addr_list[0])->s_addr;

  co::CoroutineScheduler scheduler;
  std::set<std::unique_ptr<co::Coroutine>> jobs;
  scheduler.SetCompletionCallback([&jobs](co::Coroutine *c) {
    for (auto it = jobs.begin(); it != jobs.end(); it++) {
      if (it->get() == c) {
        jobs.erase(it);
        return;
      }
    }
  });

  // Run all the jobs in parallel.  They will be removed from the
  // jobs set when they complete.
  for (int i = 0; i < num_jobs; i++) {
    jobs.insert(std::make_unique<co::Coroutine>(
        scheduler, [ipaddr, host, filename](co::Coroutine *c) {
          Client(c, host, ipaddr, filename);
        }));
  }

  // Run the main loop
  scheduler.Run();
}
