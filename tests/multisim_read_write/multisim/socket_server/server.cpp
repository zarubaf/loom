#include "server.h"
#include <string>

std::set<char const *> Server::serverNameSet;

Server::Server(char const *server_info_dir, char const *name)
    : serverInfoDir(server_info_dir), serverName(name) {
  char *name_copy = new char[SERVERNAME_MAX_SIZE];
  strcpy(name_copy, name);
  if (Server::serverNameSet.find(name_copy) != Server::serverNameSet.end()) {
    fprintf(stderr, "ERROR: server name [%s] already exist, use another name\n", name_copy);
    exit(EXIT_FAILURE);
  }
  Server::serverNameSet.insert(name_copy);
}

void Server::start() {
  FILE *fp;
  std::string server_info_file;

  // create server
  if (serverIsRunning) {
    fprintf(stderr, "ERROR: server [%s] start() has already been called\n", serverName);
    exit(EXIT_FAILURE);
  }
  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  // Allow immediate port reuse after process exit (avoids TIME_WAIT conflicts)
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  fcntl(server_fd, F_SETFL, O_NONBLOCK);
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;  // OS-assigned ephemeral port
  if (::bind(server_fd, (struct sockaddr *)&address, addrlen) < 0) {
    perror("bind");
    exit(EXIT_FAILURE);
  }

  // Read back the assigned port
  struct sockaddr_in bound_addr;
  socklen_t bound_len = sizeof(bound_addr);
  if (getsockname(server_fd, (struct sockaddr *)&bound_addr, &bound_len) < 0) {
    perror("getsockname");
    exit(EXIT_FAILURE);
  }
  serverPort = ntohs(bound_addr.sin_port);

  if (listen(server_fd, 8) < 0) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  serverIp = "127.0.0.1";
  serverIsRunning = true;

  // print server's ip and port
  mkdir(serverInfoDir, 0777);
  server_info_file = std::string(serverInfoDir) + "/server_" + std::string(serverName) + ".txt";
  fp = fopen(server_info_file.c_str(), "w+");
  fprintf(fp, "ip: %s\n", serverIp);
  fprintf(fp, "port: %0d\n", serverPort);
  fflush(fp);
  fclose(fp);
  printf("Server: [%s] has started on port %d, info in %s\n", serverName, serverPort,
         server_info_file.c_str());
}

int Server::acceptNewSocket() {
  int new_socket;
  new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
  fcntl(new_socket, F_SETFL, O_NONBLOCK);
  return new_socket;
}
