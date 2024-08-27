// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "coroutine.h"
#include <csignal>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

static co::CoroutineScheduler *g_scheduler;
void Signal(int sig) {
  printf("\nAll coroutines:\n");
  g_scheduler->Show();
  signal(sig, SIG_DFL);
  raise(sig);
}

// Send a buffer full of data to the coroutines file descriptor.
static void SendToClient(co::Coroutine *c, int fd, const char *response,
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
    ssize_t n = write(fd, response + offset, nbytes);
    if (n == -1) {
      perror("write");
      return;
    }
    if (n == 0) {
      return;
    }
    length -= n;
    offset += n;
  }
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

static void ReadHeaders(std::string &buffer, std::vector<std::string> &header,
                        std::map<std::string, std::string> &http_headers) {
  // Parse the header.
  size_t i = 0;
  // Find the \r\n at the end of the first line.
  while (i < buffer.size() && buffer[i] != '\r') {
    i++;
  }
  if (i == buffer.size()) {
    // No header line.
    return;
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
}

void Server(co::Coroutine *c, int fd, struct sockaddr_in sender,
            socklen_t sender_len) {
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

    // A blank line terminates the read
    if (buffer.find("\r\n\r\n") != std::string::npos) {
      break;
    }
  }

  std::vector<std::string> header;
  std::map<std::string, std::string> http_headers;

  // The buffer contains the HTTP header line and the HTTP headers.
  ReadHeaders(buffer, header, http_headers);

  // These are the indexes into the http_header for the fields.
  const size_t kMethod = 0;
  const size_t kFilename = 1;
  const size_t kProtocol = 2;

  // Make alises for the http header fields.
  std::string &method = header[kMethod];
  std::string &filename = header[kFilename];
  std::string &protocol = header[kProtocol];

  char response[256];

  std::string hostname = "unknown";
  auto it = http_headers.find("HOST");
  if (it != http_headers.end()) {
    hostname = it->second;
  }

  printf("%s: %s for %s from %s\n", c->Name().c_str(), method.c_str(),
         filename.c_str(), hostname.c_str());

  // Only support the GET method for now.

  if (method == "GET") {
    struct stat st;
    int e = stat(filename.c_str(), &st);
    if (e == -1) {
      int n = snprintf(response, sizeof(response), "%s 404 Not Found\r\n\r\n",
                       protocol.c_str());
      SendToClient(c, fd, response, n);
    } else {
      int file_fd = open(filename.c_str(), O_RDONLY);
      if (file_fd == -1) {
        int n = snprintf(response, sizeof(response), "%s 404 Not Found\r\n\r\n",
                         protocol.c_str());
        SendToClient(c, fd, response, n);
      } else {
        // Send the file back.
        int n =
            snprintf(response, sizeof(response),
                     "%s 200 OK\r\nContent-type: text/html\r\nContent-length: "
                     "%zd\r\n\r\n",
                     protocol.c_str(), static_cast<size_t>(st.st_size));
        SendToClient(c, fd, response, n);

        for (;;) {
          char buf[1024];
          c->Wait(file_fd, POLLIN);
          ssize_t n = read(file_fd, buf, sizeof(buf));
          if (n == -1) {
            perror("file read");
            break;
          }
          if (n == 0) {
            break;
          }
          SendToClient(c, fd, buf, n);
        }
        close(file_fd);
      }
    }
  } else {
    // Invalid request method.
    int n = snprintf(response, sizeof(response),
                     "%s 400 Invalid request method\r\n\r\n", protocol.c_str());
    SendToClient(c, fd, response, n);
  }

  close(fd);
}

void Listener(co::Coroutine *c) {
  int s = socket(PF_INET, SOCK_STREAM, 0);
  if (s == -1) {
    perror("socket");
    return;
  }
  int val = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(80),
#if defined(__APPLE__)
                             .sin_len = sizeof(int),
#endif
                             .sin_addr = {.s_addr = INADDR_ANY}};
  int e = bind(s, (struct sockaddr *)&addr, sizeof(addr));
  if (e == -1) {
    perror("bind");
    close(s);
    return;
  }
  listen(s, 10);

  std::set<std::unique_ptr<co::Coroutine>> coroutines;

  c->Scheduler().SetCompletionCallback([&coroutines](co::Coroutine *c) {
    for (auto it = coroutines.begin(); it != coroutines.end(); it++) {
      if (it->get() == c) {
        coroutines.erase(it);
        return;
      }
    }
  });

  // Enter a loop accepting incoming connections and spawning coroutines
  // to handle each one.  All coroutines run "in parallel", cooperating with
  // each other.  No threading here.
  for (;;) {
    // Wait for incoming connection.  This allows other coroutines to run
    // while we are waiting.
    c->Wait(s, POLLIN);

    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    int fd = accept(s, (struct sockaddr *)&sender, &sender_len);
    if (fd == -1) {
      perror("accept");
      continue;
    }

    // Make a coroutine to handle the connection.
    coroutines.insert(std::make_unique<co::Coroutine>(
        c->Scheduler(), [fd, sender, sender_len](co::Coroutine *c) {
          Server(c, fd, sender, sender_len);
        }));
  }
}

int main(int argc, const char *argv[]) {
  co::CoroutineScheduler scheduler;

  g_scheduler = &scheduler; // For signal handler.
  signal(SIGPIPE, SIG_IGN);
  signal(SIGQUIT, Signal);

  co::Coroutine listener(scheduler, Listener, "listener");

  // Run the main loop
  scheduler.Run();
}
