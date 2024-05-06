/*
   BAREOS® - Backup Archiving REcovery Open Sourced

Copyright (C) 2023-2023 Bareos GmbH & Co. KG

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

#define NEED_JANSSON_NAMESPACE 1


#include "dird/dird_globals.h"
#include "dird_conf.h"
#include "dird.h"
#include "lib/parse_conf.h"

#include "websocketjsonrpcserver.h"
#include "websocketpp/uri.hpp"

#include <sstream>

typedef websocketpp::server<websocketpp::config::asio> server;

using namespace jsonrpc;
using namespace directordaemon;

// Define a callback to handle incoming messages
void WebsocketJsonRpcServer::on_message(wsasioserver* wsserver,
                                        websocketpp::connection_hdl hdl,
                                        message_ptr msg)
{
  try {
    std::string response;
    ProcessRequest(msg->get_payload(), response);
    wsserver->send(hdl, response, msg->get_opcode());
  } catch (websocketpp::exception const& e) {
    std::cout << "Operation failed because: "
              << "(" << e.what() << ")" << std::endl;
  }
}

void WebsocketJsonRpcServer::on_open(wsasioserver* wsserver,
                                     websocketpp::connection_hdl hdl)
{
  try {
    wsserver->send(hdl,
                   "{ \"jsonrpc\": \"2.0\", \"notification\": \"jsonrpc server ready\"}",
                   websocketpp::frame::opcode::text);
  } catch (websocketpp::exception const& e) {
    std::cout << "Operation failed because: "
              << "(" << e.what() << ")" << std::endl;
  }
}

void WebsocketJsonRpcServer::on_close(wsasioserver* wsserver,
                                      websocketpp::connection_hdl hdl)
{
  try {
    wsserver->send(hdl, "Ciao!", websocketpp::frame::opcode::text);
  } catch (websocketpp::exception const& e) {
    std::cout << "Operation failed because: "
              << "(" << e.what() << ")" << std::endl;
  }
}

bool WebsocketJsonRpcServer::on_validate(wsasioserver* wsserver,
                                      websocketpp::connection_hdl hdl)
{
  try {
    server::connection_ptr cp = wsserver->get_con_from_hdl(hdl);
    websocketpp::uri_ptr request_uri = cp->get_uri();
    std::string q = request_uri->get_query();

    std::stringstream sstr(q);
    std::string username,password;

    if(getline(sstr, username, '_') && getline(sstr, password, '_'))
    {
      ConsoleResource *cons = dynamic_cast<ConsoleResource*>(
		my_config->GetResWithName(R_CONSOLE, username.c_str()));

      if(cons->password_.value == password)
        return true;

      if((directordaemon::me->password_.encoding == p_encoding_md5)
          && (password == directordaemon::me->password_.value)) {
          return true;
      }
    }
    else {
        std::cout << "Authentication failed" << std::endl;
        return true;
    }
  } catch (websocketpp::exception const& e) {
    std::cout << "Operation failed because: "
              << "(" << e.what() << ")" << std::endl;
  }
  return true;
}

WebsocketJsonRpcServer::WebsocketJsonRpcServer(int port)
    : AbstractServerConnector(), port_(port)
{
}

bool WebsocketJsonRpcServer::StartListening()
{
  listenning_thread = std::thread([this]() { StartWebsocket(); });
  return true;
}

bool WebsocketJsonRpcServer::StopListening()
{
  StopWebsocket();
  return true;
}

void WebsocketJsonRpcServer::StopWebsocket() { listenning_thread.join(); }

void WebsocketJsonRpcServer::StartWebsocket()
{
  wsserver_.set_access_channels(websocketpp::log::alevel::all);
  wsserver_.clear_access_channels(websocketpp::log::alevel::frame_payload);

  wsserver_.init_asio();

  wsserver_.set_message_handler(
      bind(&WebsocketJsonRpcServer::on_message, this, &wsserver_, ::_1, ::_2));
  wsserver_.set_open_handler(
      bind(&WebsocketJsonRpcServer::on_open, this, &wsserver_, ::_1));
  wsserver_.set_close_handler(
      bind(&WebsocketJsonRpcServer::on_close, this, &wsserver_, ::_1));
  wsserver_.set_validate_handler(
      bind(&WebsocketJsonRpcServer::on_validate, this, &wsserver_, ::_1));
  try {
    wsserver_.listen(port_);
    // Start the server accept loop
    wsserver_.start_accept();
  } catch (const std::exception& error) {
    Jmsg((JobControlRecord*)nullptr, M_WARNING, 0,
         _("Could not start RPC_server; what(): %s\n"), error.what());

    return;
  }

  wsserver_.run();
}
