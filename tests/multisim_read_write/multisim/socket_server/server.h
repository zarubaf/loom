#ifndef SERVER_H
#define SERVER_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SERVERNAME_MAX_SIZE 200

/**
 * Implementation of a non-blocking socket server
 *
 * Main features:
 *  - binds to an OS-assigned ephemeral port on localhost
 *  - write port and ip info in 'server_<serverName>.txt' at start()
 *  - forbids servers with the same name
 */
class Server {
public:
  Server(char const *server_info_dir, char const *name);
  void start();
  int acceptNewSocket();
  char const *serverName;
  char const *serverInfoDir;
  char const *serverIp;
  int serverPort;

private:
  int server_fd;
  struct sockaddr_in address;
  int addrlen = sizeof(address);
  bool serverIsRunning = false;
  static std::set<char const *> serverNameSet;
};
#endif
