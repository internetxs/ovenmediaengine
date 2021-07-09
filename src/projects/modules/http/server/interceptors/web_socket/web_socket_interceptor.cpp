//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "./web_socket_interceptor.h"

#include <base/ovcrypto/ovcrypto.h>
#include <modules/http/server/http_server.h>

#include <utility>

#include "../../../http_private.h"
#include "./web_socket_datastructure.h"
#include "./web_socket_frame.h"

namespace http
{
	namespace svr
	{
		namespace ws
		{
			Interceptor::Interceptor()
			{
				_ping_timer.Push(std::bind(&Interceptor::DoPing, this, std::placeholders::_1), 30 * 1000);
				_ping_timer.Start();
			}

			Interceptor::~Interceptor()
			{
				_ping_timer.Stop();
			}

			ov::DelayQueueAction Interceptor::DoPing(void *parameter)
			{
				{
					std::shared_lock<std::shared_mutex> lock_guard(_websocket_client_list_mutex);

					if (_websocket_client_list.size() > 0)
					{
						logtd("Trying to ping to WebSocket clients...");

						ov::String str("OvenMediaEngine");
						auto payload = std::move(str.ToData(false));

						for (auto client : _websocket_client_list)
						{
							client.second->response->Send(payload, FrameOpcode::Ping);
						}
					}
				}

				return ov::DelayQueueAction::Repeat;
			}

			bool Interceptor::IsInterceptorForRequest(const std::shared_ptr<const HttpConnection> &client)
			{
				const auto request = client->GetRequest();

				if (request->GetConnectionType() != RequestConnectionType::WebSocket)
				{
					logtd("%s is not websocket request", request->ToString().CStr());
					return false;
				}

				return true;
			}

			InterceptorResult Interceptor::OnHttpPrepare(const std::shared_ptr<HttpConnection> &client)
			{
				auto request = client->GetRequest();
				auto response = client->GetResponse();

				// RFC6455 - 4.2.2.  Sending the Server's Opening Handshake
				response->SetStatusCode(StatusCode::SwitchingProtocols);

				response->SetHeader("Upgrade", "websocket");
				response->SetHeader("Connection", "Upgrade");

				// 4.  A |Sec-WebSocket-Accept| header field.  The value of this
				//    header field is constructed by concatenating /key/, defined
				//    above in step 4 in Section 4.2.2, with the string "258EAFA5-
				//    E914-47DA-95CA-C5AB0DC85B11", taking the SHA-1 hash of this
				//    concatenated value to obtain a 20-byte value and base64-
				//    encoding (see Section 4 of [RFC4648]) this 20-byte hash.
				const ov::String unique_id = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
				ov::String key = request->GetHeader("SEC-WEBSOCKET-KEY");

				std::shared_ptr<ov::Data> hash = ov::MessageDigest::ComputeDigest(ov::CryptoAlgorithm::Sha1, (key + unique_id).ToData(false));
				ov::String base64 = ov::Base64::Encode(hash);

				response->SetHeader("Sec-WebSocket-Accept", base64);

				// client에 헤더 전송
				response->Response();

				// 지속적으로 통신해야 하므로, 연결은 끊지 않음
				logtd("Add to websocket client list: %s", request->ToString().CStr());
				auto websocket_response = std::make_shared<Client>(client);

				{
					std::lock_guard<std::shared_mutex> lock_guard(_websocket_client_list_mutex);

					_websocket_client_list.emplace(request, std::make_shared<Info>(websocket_response, nullptr));
				}

				if (_connection_handler != nullptr)
				{
					return _connection_handler(websocket_response);
				}

				return InterceptorResult::Keep;
			}

			InterceptorResult Interceptor::OnHttpData(const std::shared_ptr<HttpConnection> &client, const std::shared_ptr<const ov::Data> &data)
			{
				auto request = client->GetRequest();

				if (data->GetLength() == 0)
				{
					// Nothing to do
					return InterceptorResult::Keep;
				}

				std::shared_ptr<Info> info = nullptr;

				{
					std::shared_lock<std::shared_mutex> lock_guard(_websocket_client_list_mutex);

					auto item = _websocket_client_list.find(request);

					if (item == _websocket_client_list.end())
					{
						// TODO(dimiden): Temporarily comment out assertions due to side-effect on socket side modification
						// OV_ASSERT2(false);
						return InterceptorResult::Disconnect;
					}

					info = item->second;
				}

				if (info == nullptr)
				{
					OV_ASSERT2(false);
					return InterceptorResult::Disconnect;
				}

				logtd("Data is received\n%s", data->Dump().CStr());

				if (info->frame == nullptr)
				{
					info->frame = std::make_shared<Frame>();
				}

				auto frame = info->frame;
				auto processed_bytes = frame->Process(data);

				switch (frame->GetStatus())
				{
					case FrameParseStatus::Prepare:
						// Not enough data to parse header
						break;

					case FrameParseStatus::Parsing:
						break;

					case FrameParseStatus::Completed: {
						const std::shared_ptr<const ov::Data> payload = frame->GetPayload();

						switch (static_cast<FrameOpcode>(frame->GetHeader().opcode))
						{
							case FrameOpcode::ConnectionClose:
								// 접속 종료 요청됨
								logtd("Client requested close connection: reason:\n%s", payload->Dump("Reason").CStr());
								return InterceptorResult::Disconnect;

							case FrameOpcode::Ping:
								logtd("A ping frame is received:\n%s", payload->Dump().CStr());

								info->frame = nullptr;

								// Send a pong frame to the client
								info->response->Send(payload, FrameOpcode::Pong);

								return InterceptorResult::Keep;

							case FrameOpcode::Pong:
								// Ignore pong frame
								logtd("A pong frame is received:\n%s", payload->Dump().CStr());

								info->frame = nullptr;

								return InterceptorResult::Keep;

							default:
								logtd("%s:\n%s", frame->ToString().CStr(), payload->Dump("Frame", 0L, 1024L, nullptr).CStr());

								// 패킷 조립이 완료되었음
								// 상위 레벨로 올림
								if (_message_handler != nullptr)
								{
									if (payload->GetLength() > 0L)
									{
										// 데이터가 있을 경우에만 올림
										if (_message_handler(info->response, frame) == InterceptorResult::Disconnect)
										{
											return InterceptorResult::Disconnect;
										}
									}

									info->frame = nullptr;
								}

								// 나머지 데이터로 다시 파싱 시작
								OV_ASSERT2(processed_bytes >= 0L);

								if (processed_bytes > 0L)
								{
									return OnHttpData(client, data->Subdata(processed_bytes));
								}
						}

						break;
					}

					case FrameParseStatus::Error:
						// 잘못된 데이터가 수신되었음 WebSocket 연결을 해제함
						logtw("Invalid data received from %s", request->ToString().CStr());
						return InterceptorResult::Disconnect;
				}

				return InterceptorResult::Keep;
			}

			void Interceptor::OnHttpError(const std::shared_ptr<HttpConnection> &client, StatusCode status_code)
			{
				auto request = client->GetRequest();
				auto response = client->GetResponse();

				std::shared_ptr<Info> socket_info;

				{
					std::lock_guard<std::shared_mutex> lock_guard(_websocket_client_list_mutex);

					auto item = _websocket_client_list.find(request);

					logtd("An error occurred: %s...", request->ToString().CStr());

					OV_ASSERT2(item != _websocket_client_list.end());

					socket_info = item->second;

					_websocket_client_list.erase(item);
				}

				if ((_error_handler != nullptr) && (socket_info != nullptr))
				{
					_error_handler(socket_info->response, ov::Error::CreateError(static_cast<int>(status_code), "%s", StringFromStatusCode(status_code)));
				}

				response->SetStatusCode(status_code);
			}

			void Interceptor::OnHttpClosed(const std::shared_ptr<HttpConnection> &client, PhysicalPortDisconnectReason reason)
			{
				auto request = client->GetRequest();

				std::shared_ptr<Info> socket_info;
				{
					std::lock_guard<std::shared_mutex> lock_guard(_websocket_client_list_mutex);

					auto item = _websocket_client_list.find(request);

					logtd("Deleting %s from websocket client list...", request->ToString().CStr());

					if (item != _websocket_client_list.end())
					{
						socket_info = item->second;
						_websocket_client_list.erase(item);
					}
					else
					{
						// TODO(dimiden): Temporarily comment out assertions due to side-effect on socket side modification
						// OV_ASSERT2(item != _websocket_client_list.end());
					}
				}

				if ((_close_handler != nullptr) && (socket_info != nullptr))
				{
					_close_handler(socket_info->response, reason);
				}
			}

			void Interceptor::SetConnectionHandler(ConnectionHandler handler)
			{
				_connection_handler = std::move(handler);
			}

			void Interceptor::SetMessageHandler(MessageHandler handler)
			{
				_message_handler = std::move(handler);
			}

			void Interceptor::SetErrorHandler(ErrorHandler handler)
			{
				_error_handler = std::move(handler);
			}

			void Interceptor::SetCloseHandler(CloseHandler handler)
			{
				_close_handler = std::move(handler);
			}
		}  // namespace ws
	}	   // namespace svr
}  // namespace http
