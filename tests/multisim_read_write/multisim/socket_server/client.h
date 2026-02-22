#ifndef CLIENT_H
#define CLIENT_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * Implementation of a non-blocking socket client
 */
class Client {
public:
  Client(char const *server_info_dir, char const *name);
  void start();
  int startWithAddressAndPort(char const *server_address, int server_port);
  int getSocket();
  char const *clientIp;
  char const *serverName;
  char const *serverInfoDir;
  char const *serverIp;
  int serverPort;

private:
  int new_socket;
  int getServerIpAndPort(char const *server_file);
};
#endif
