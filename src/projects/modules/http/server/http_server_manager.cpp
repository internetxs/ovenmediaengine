//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2020 AirenSoft. All rights reserved.
//
//==============================================================================
#include "http_server_manager.h"

#include <orchestrator/orchestrator.h>

#include "../http_private.h"

namespace http
{
	namespace svr
	{
		std::shared_ptr<HttpServer> HttpServerManager::CreateHttpServer(const char *server_name, const ov::SocketAddress &address, int worker_count)
		{
			std::shared_ptr<HttpServer> http_server = nullptr;

			{
				auto lock_guard = std::lock_guard(_http_servers_mutex);
				auto item = _http_servers.find(address);

				if (item != _http_servers.end())
				{
					http_server = item->second;

					// Assume that http_server is not HttpsServer
					auto https_server = std::dynamic_pointer_cast<HttpsServer>(http_server);

					if (https_server != nullptr)
					{
						logte("Cannot reuse instance: Requested Server, but previous instance is HttpsServer (%s)", address.ToString().CStr());
						http_server = nullptr;
					}
					else
					{
						if (worker_count != HTTP_SERVER_USE_DEFAULT_COUNT)
						{
							auto physical_port = http_server->GetPhysicalPort();

							if (physical_port != nullptr)
							{
								if (physical_port->GetWorkerCount() != worker_count)
								{
									logtw("The number of workers in the existing physical port differs from the number of workers passed by the argument: physical port: %zu, argument: %zu",
										  physical_port->GetWorkerCount(), worker_count);
									logtw("Because worker counts are different, the first initialized count is used: %d", physical_port->GetWorkerCount());
								}
							}
						}
					}
				}
				else
				{
					// Create a new HTTP server
					http_server = std::make_shared<HttpServer>(server_name);

					if (http_server->Start(address, worker_count))
					{
						_http_servers[address] = http_server;
					}
					else
					{
						// Failed to start
						http_server = nullptr;
					}
				}

				return http_server;
			}
		}

		std::shared_ptr<HttpsServer> HttpServerManager::CreateHttpsServer(const char *server_name, const ov::SocketAddress &address, const std::shared_ptr<info::Certificate> &certificate, int worker_count)
		{
			std::shared_ptr<HttpsServer> https_server = nullptr;

			{
				auto lock_guard = std::lock_guard(_http_servers_mutex);
				auto item = _http_servers.find(address);

				if (item != _http_servers.end())
				{
					auto http_server = item->second;

					// Assume that http_server is not HttpsServer
					https_server = std::dynamic_pointer_cast<HttpsServer>(http_server);

					if (https_server == nullptr)
					{
						logte("Cannot reuse instance: Requested HttpsServer, but previous instance is Server (%s)", address.ToString().CStr());
					}
					else if (https_server->SetCertificate(certificate) == false)
					{
						logte("Could not set certificate: HttpsServer can use only one certificate at the same time");
						https_server = nullptr;
					}
					else
					{
						// HTTPS server is ready
					}
				}
				else
				{
					// Create a new HTTP server
					https_server = std::make_shared<HttpsServer>(server_name);

					if (https_server->SetCertificate(certificate))
					{
						if (https_server->Start(address, worker_count))
						{
							_http_servers[address] = https_server;
						}
						else
						{
							// Failed to start HTTP server
							https_server = nullptr;
						}
					}
					else
					{
						logte("Could not set certificate: HttpsServer can use only one certificate at the same time");
						https_server = nullptr;
					}
				}

				return https_server;
			}
		}

		std::shared_ptr<HttpsServer> HttpServerManager::CreateHttpsServer(const char *server_name, const ov::SocketAddress &address, const std::vector<std::shared_ptr<ocst::VirtualHost>> &virtual_host_list, int worker_count)
		{
			// Check if TLS is enabled
			auto vhost_list = ocst::Orchestrator::GetInstance()->GetVirtualHostList();

			if (vhost_list.empty())
			{
				return nullptr;
			}

			// TODO(Dimiden): OME doesn't support SNI yet, so OME can handle only one certificate.
			const auto &host_info = vhost_list[0]->host_info;

			return CreateHttpsServer(server_name, address, host_info.GetCertificate(), worker_count);
		}

		bool HttpServerManager::ReleaseServer(const std::shared_ptr<HttpServer> &http_server)
		{
			// TODO(dimiden): Need to implement release feature (by reference count)
			if (http_server != nullptr)
			{
				return http_server->Stop();
			}

			return false;
		}
	}  // namespace svr
}  // namespace http
