/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#include "events.pb.h"
#include "plugin.grpc.pb.h"
#include "plugin.pb.h"
#include "bareos.grpc.pb.h"
#include "bareos.pb.h"
#include "filed/fd_plugins.h"
#include <grpcpp/grpcpp.h>
#include <grpc/grpc_posix.h>
#include <grpc/grpc_security.h>

#include <fmt/format.h>

#include "plugin_service.h"

namespace bc = bareos::core;

struct BareosClient {
  BareosClient(std::shared_ptr<grpc::ChannelInterface> channel)
      : stub_(bc::Events::NewStub(channel))
  {
  }

  std::unique_ptr<bc::Events::Stub> stub_;
};

struct grpc_connection {
  BareosClient client;
  std::unique_ptr<grpc::Server> server;
  std::vector<std::unique_ptr<grpc::Service>> services;

  grpc_connection() = delete;
};

struct connection_builder {
  std::optional<BareosClient> opt_client;
  std::optional<std::unique_ptr<grpc::Server>> opt_server;
  std::vector<std::unique_ptr<grpc::Service>> services;

  template <typename... Args> connection_builder(Args&&... args)
  {
    (services.emplace_back(std::forward<Args>(args)), ...);
  }

  connection_builder& connect_client(int sockfd)
  {
    auto* channel = grpc_channel_create_from_fd(
        "plugin", sockfd, grpc_insecure_credentials_create(), nullptr);

    if (channel) {
      // DebugLog(100, FMT_STRING("could connect to client over socket {}"),
      //          sockfd);

      opt_client.emplace(grpc::CreateChannelInternal("", channel, {}));
    } else {
      // DebugLog(50, FMT_STRING("could not connect to client over socket {}"),
      //          sockfd);
    }

    return *this;
  }

  connection_builder& connect_server(int sockfd)
  {
    try {
      grpc::ServerBuilder builder;

      for (auto& service : services) { builder.RegisterService(service.get()); }

      auto ccserver = builder.BuildAndStart();

      auto* server = ccserver->c_server();

      grpc_server_add_channel_from_fd(
          server, sockfd, grpc_insecure_server_credentials_create());
    } catch (const std::exception& e) {
      // DebugLog(50, FMT_STRING("could not attach socket {} to server:
      // Err={}"),
      //          sockfd, e.what());
      opt_server = std::nullopt;
    } catch (...) {
      opt_server = std::nullopt;
    }
    return *this;
  }


  std::optional<grpc_connection> build()
  {
    if (!opt_client) { return std::nullopt; }
    if (!opt_server) { return std::nullopt; }

    grpc_connection con{std::move(opt_client.value()),
                        std::move(opt_server.value()), std::move(services)};

    return con;
  }
};

#include <fcntl.h>

std::optional<grpc_connection> con;

void HandleConnection(int server_sock, int client_sock)
{
  grpc_init();

  con = connection_builder{std::make_unique<PluginService>()}
            .connect_server(server_sock)
            .connect_client(client_sock)
            .build();

  if (!con) { exit(1); }

  con->server->Wait();

  grpc_shutdown();
}


int main(int argc, char* argv[])
{
  if (argc != 3) { return 5; }
  int sock1 = atoi(argv[1]);
  int sock2 = atoi(argv[2]);

  if (fcntl(sock1, F_GETFD) < 0) {
    printf("bad file descriptor given: %d\n", sock1);
    return 3;
  }

  if (fcntl(sock2, F_GETFD) < 0) {
    printf("bad file descriptor given: %d\n", sock2);
    return 3;
  }

  HandleConnection(sock1, sock2);

  close(sock1);
  close(sock2);
}
