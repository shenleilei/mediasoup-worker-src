#define MS_CLASS "Channel::ChannelSocket"
// #define MS_LOG_DEV_LEVEL 3

#include "Channel/ChannelSocket.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include <cstring> // std::memcpy(), std::memmove()
#include <memory>

namespace Channel
{
	// Binary length for a 4194304 bytes payload.
	static constexpr size_t MessageMaxLen{ 4194308 };
	static constexpr size_t PayloadMaxLen{ 4194304 };

#ifdef MS_TEST
	namespace
	{
		thread_local bool failNextAsyncSendForTesting{ false };
		thread_local size_t asyncCloseCountForTesting{ 0u };
	}
#endif

	/* Static methods for UV callbacks. */

	inline static void onAsync(uv_handle_t* handle)
	{
		auto* channel = static_cast<ChannelSocket*>(handle->data);

		while (channel && channel->CallbackRead())
		{
			// Read while there are new messages.
		}
	}

	inline static void onCloseAsync(uv_handle_t* handle)
	{
#ifdef MS_TEST
		++asyncCloseCountForTesting;
#endif
		delete reinterpret_cast<uv_async_t*>(handle);
	}

	/* Instance methods. */

	ChannelSocket::ChannelSocket(int consumerFd, int producerFd)
	{
		MS_TRACE_STD();

		// Keep both handles locally owned until every throwing construction step
		// has completed. A producer failure must not strand the already-active
		// consumer handle with a listener pointer to this incomplete object.
		auto consumer = std::make_unique<ConsumerSocket>(consumerFd, MessageMaxLen, this);
		auto producer = std::make_unique<ProducerSocket>(producerFd, MessageMaxLen);

		this->consumerSocket = consumer.release();
		this->producerSocket = producer.release();
	}

	ChannelSocket::ChannelSocket(
	  ChannelReadFn channelReadFn,
	  ChannelReadCtx channelReadCtx,
	  ChannelWriteFn channelWriteFn,
	  ChannelWriteCtx channelWriteCtx)
	  : channelReadFn(channelReadFn), channelReadCtx(channelReadCtx), channelWriteFn(channelWriteFn),
	    channelWriteCtx(channelWriteCtx), uvReadHandle(new uv_async_t)
	{
		MS_TRACE_STD();

		int err;

		this->uvReadHandle->data = static_cast<void*>(this);

		err =
		  uv_async_init(DepLibUV::GetLoop(), this->uvReadHandle, reinterpret_cast<uv_async_cb>(onAsync));

		if (err != 0)
		{
			delete this->uvReadHandle;
			this->uvReadHandle = nullptr;

			MS_THROW_ERROR_STD("uv_async_init() failed: %s", uv_strerror(err));
		}

#ifdef MS_TEST
		if (failNextAsyncSendForTesting)
		{
			failNextAsyncSendForTesting = false;
			err                         = UV_EINVAL;
		}
		else
#endif
		{
			err = uv_async_send(this->uvReadHandle);
		}

		if (err != 0)
		{
			this->uvReadHandle->data = nullptr;
			uv_close(
			  reinterpret_cast<uv_handle_t*>(this->uvReadHandle),
			  static_cast<uv_close_cb>(onCloseAsync));
			this->uvReadHandle = nullptr;

			MS_THROW_ERROR_STD("uv_async_send() failed: %s", uv_strerror(err));
		}
	}

	ChannelSocket::~ChannelSocket()
	{
		MS_TRACE_STD();

		if (!this->closed)
		{
			Close();
		}

		delete this->consumerSocket;
		delete this->producerSocket;
	}

	void ChannelSocket::Close() noexcept
	{
		MS_TRACE_STD();

		if (this->closed)
		{
			return;
		}

		this->closed = true;

		if (this->uvReadHandle)
		{
			this->uvReadHandle->data = nullptr;
			uv_close(
			  reinterpret_cast<uv_handle_t*>(this->uvReadHandle), static_cast<uv_close_cb>(onCloseAsync));
		}

		if (this->consumerSocket)
		{
			this->consumerSocket->Close();
		}

		if (this->producerSocket)
		{
			this->producerSocket->Close();
		}
	}

#ifdef MS_TEST
	void ChannelSocket::FailNextAsyncSendForTesting()
	{
		failNextAsyncSendForTesting = true;
	}

	size_t ChannelSocket::GetAsyncCloseCountForTesting()
	{
		return asyncCloseCountForTesting;
	}
#endif

	void ChannelSocket::SetListener(Listener* listener)
	{
		MS_TRACE_STD();

		this->listener = listener;
	}

	void ChannelSocket::Send(const uint8_t* data, uint32_t dataLen)
	{
		MS_TRACE_STD();

		if (this->closed)
		{
			return;
		}

		if (dataLen > PayloadMaxLen)
		{
			MS_ERROR_STD("message too big");

			return;
		}

		SendImpl(data, dataLen);
	}

	void ChannelSocket::SendLog(const char* data, uint32_t dataLen)
	{
		MS_TRACE_STD();

		if (this->closed)
		{
			return;
		}

		if (dataLen > PayloadMaxLen)
		{
			MS_ERROR_STD("message too big");

			return;
		}

		auto log = FBS::Log::CreateLogDirect(this->bufferBuilder, data);
		auto message =
		  FBS::Message::CreateMessage(this->bufferBuilder, FBS::Message::Body::Log, log.Union());

		this->bufferBuilder.FinishSizePrefixed(message);
		this->Send(this->bufferBuilder.GetBufferPointer(), this->bufferBuilder.GetSize());
		this->bufferBuilder.Reset();
	}

	bool ChannelSocket::CallbackRead()
	{
		MS_TRACE_STD();

		if (this->closed)
		{
			return false;
		}

		uint8_t* msg{ nullptr };
		uint32_t msgLen;
		size_t msgCtx;

		// Try to read next message using `channelReadFn`, message, its length and context will be
		// stored in provided arguments.
		auto free = this->channelReadFn(&msg, &msgLen, &msgCtx, this->uvReadHandle, this->channelReadCtx);

		// Non-null free function pointer means message was successfully read above and will need to be
		// freed later.
		if (free)
		{
			try
			{
				const auto* message = FBS::Message::GetMessage(msg);

#if MS_LOG_DEV_LEVEL == 3
				auto s = flatbuffers::FlatBufferToString(
				  reinterpret_cast<uint8_t*>(msg), FBS::Message::MessageTypeTable());
				MS_DUMP("%s", s.c_str());
#endif

				ProcessMessage(message);
			}
			catch (const std::exception& error)
			{
				MS_ERROR("channel message callback failed: %s", error.what());
				FailClosed();
			}
			catch (...)
			{
				MS_ERROR("channel message callback failed with an unknown exception");
				FailClosed();
			}

			// Message needs to be freed using stored function pointer.
			free(msg, msgLen, msgCtx);
		}

		// Return `true` if something was processed.
		return free != nullptr;
	}

	bool ChannelSocket::RejectRequest(ChannelRequest* request, bool typeError, const char* reason) noexcept
	{
		if (!request)
		{
			return false;
		}

		if (request->replied)
		{
			MS_ERROR("request handler failed after sending its response [method:%s]", request->methodCStr);

			return false;
		}

		try
		{
			if (typeError)
			{
				request->TypeError(reason);
			}
			else
			{
				request->Error(reason);
			}

			return true;
		}
		catch (const std::exception& error)
		{
			MS_ERROR("failed to send request rejection: %s", error.what());
		}
		catch (...)
		{
			MS_ERROR("failed to send request rejection with an unknown exception");
		}

		return false;
	}

	void ChannelSocket::FailClosed() noexcept
	{
		if (this->closed)
		{
			return;
		}

		Close();

		if (!this->listener)
		{
			return;
		}

		try
		{
			this->listener->OnChannelClosed(this);
		}
		catch (const std::exception& error)
		{
			MS_ERROR("channel close listener failed: %s", error.what());
		}
		catch (...)
		{
			MS_ERROR("channel close listener failed with an unknown exception");
		}
	}

	void ChannelSocket::ProcessMessage(const FBS::Message::Message* message)
	{
		if (!message)
		{
			MS_ERROR("discarding null Channel message");
			FailClosed();

			return;
		}

		if (message->data_type() == FBS::Message::Body::Request)
		{
			const auto* requestData = message->data_as<FBS::Request::Request>();
			std::unique_ptr<ChannelRequest> request;

			if (!requestData)
			{
				MS_ERROR("discarding Channel request without data");
				FailClosed();

				return;
			}

			try
			{
				request = std::make_unique<ChannelRequest>(this, requestData);

				if (!this->listener)
				{
					MS_THROW_ERROR("channel request received before listener registration");
				}

				this->listener->HandleRequest(request.get());
			}
			catch (const MediaSoupTypeError& error)
			{
				if (!RejectRequest(request.get(), true, error.what()))
				{
					FailClosed();
				}
			}
			catch (const MediaSoupError& error)
			{
				if (request)
				{
					if (!RejectRequest(request.get(), false, error.what()))
					{
						FailClosed();
					}
				}
				else if (
				  !requestData || ChannelRequest::method2String.find(requestData->method()) !=
				                    ChannelRequest::method2String.end())
				{
					// A known method failed while its ChannelRequest was still being
					// constructed, so no reliable response object exists. Unknown methods
					// reject themselves in the constructor before throwing.
					MS_ERROR("request construction failed: %s", error.what());
					FailClosed();
				}
			}
			catch (const std::exception& error)
			{
				MS_ERROR("request callback failed: %s", error.what());
				RejectRequest(request.get(), false, error.what());
				FailClosed();
			}
			catch (...)
			{
				MS_ERROR("request callback failed with an unknown exception");
				RejectRequest(request.get(), false, "unknown request failure");
				FailClosed();
			}
		}
		else if (message->data_type() == FBS::Message::Body::Notification)
		{
			const auto* notificationData = message->data_as<FBS::Notification::Notification>();

			if (!notificationData)
			{
				MS_ERROR("discarding Channel notification without data");
				FailClosed();

				return;
			}

			try
			{
				auto notification = std::make_unique<ChannelNotification>(notificationData);

				if (!this->listener)
				{
					MS_THROW_ERROR("channel notification received before listener registration");
				}

				this->listener->HandleNotification(notification.get());
			}
			catch (const MediaSoupError& error)
			{
				MS_ERROR("notification failed: %s", error.what());
			}
			catch (const std::exception& error)
			{
				MS_ERROR("notification callback failed: %s", error.what());
				FailClosed();
			}
			catch (...)
			{
				MS_ERROR("notification callback failed with an unknown exception");
				FailClosed();
			}
		}
		else
		{
			MS_ERROR("discarding wrong Channel data");
		}
	}

	void ChannelSocket::SendImpl(const uint8_t* payload, uint32_t payloadLen)
	{
		MS_TRACE_STD();

		// Write using function call if provided.
		if (this->channelWriteFn)
		{
			this->channelWriteFn(payload, payloadLen, this->channelWriteCtx);
		}
		else
		{
			this->producerSocket->Write(payload, payloadLen);
		}
	}

	void ChannelSocket::OnConsumerSocketMessage(
	  ConsumerSocket* /*consumerSocket*/, char* msg, size_t /*msgLen*/)
	{
		MS_TRACE();

		try
		{
			const auto* message = FBS::Message::GetMessage(msg);

#if MS_LOG_DEV_LEVEL == 3
			auto s = flatbuffers::FlatBufferToString(
			  reinterpret_cast<uint8_t*>(msg), FBS::Message::MessageTypeTable());
			MS_DUMP("%s", s.c_str());
#endif

			ProcessMessage(message);
		}
		catch (const std::exception& error)
		{
			MS_ERROR("channel socket message callback failed: %s", error.what());
			FailClosed();
		}
		catch (...)
		{
			MS_ERROR("channel socket message callback failed with an unknown exception");
			FailClosed();
		}
	}

	void ChannelSocket::OnConsumerSocketClosed(ConsumerSocket* /*consumerSocket*/)
	{
		MS_TRACE_STD();

		FailClosed();
	}

	/* Instance methods. */

	ConsumerSocket::ConsumerSocket(int fd, size_t bufferSize, Listener* listener)
	  : ::UnixStreamSocketHandle(fd, bufferSize, ::UnixStreamSocketHandle::Role::CONSUMER),
	    listener(listener)
	{
		MS_TRACE_STD();
	}

	ConsumerSocket::~ConsumerSocket()
	{
		MS_TRACE_STD();
	}

	void ConsumerSocket::UserOnUnixStreamRead()
	{
		MS_TRACE_STD();

		size_t msgStart{ 0 };

		// Be ready to parse more than a single message in a single chunk.
		while (true)
		{
			if (IsClosed())
			{
				return;
			}

			const size_t readLen = this->bufferDataLen - msgStart;

			if (readLen < sizeof(uint32_t))
			{
				// Incomplete data.
				break;
			}

			uint32_t msgLen;
			// Read message length.
			std::memcpy(&msgLen, this->buffer + msgStart, sizeof(uint32_t));

			if (readLen < sizeof(uint32_t) + static_cast<size_t>(msgLen))
			{
				// Incomplete data.
				break;
			}

			this->listener->OnConsumerSocketMessage(
			  this,
			  reinterpret_cast<char*>(this->buffer + msgStart + sizeof(uint32_t)),
			  static_cast<size_t>(msgLen));

			msgStart += sizeof(uint32_t) + static_cast<size_t>(msgLen);
		}

		if (msgStart != 0)
		{
			this->bufferDataLen = this->bufferDataLen - msgStart;

			if (this->bufferDataLen != 0)
			{
				std::memmove(this->buffer, this->buffer + msgStart, this->bufferDataLen);
			}
		}
	}

	void ConsumerSocket::UserOnUnixStreamSocketClosed()
	{
		MS_TRACE_STD();

		// Notify the listener.
		this->listener->OnConsumerSocketClosed(this);
	}

	/* Instance methods. */

	ProducerSocket::ProducerSocket(int fd, size_t bufferSize)
	  : ::UnixStreamSocketHandle(fd, bufferSize, ::UnixStreamSocketHandle::Role::PRODUCER)
	{
		MS_TRACE_STD();
	}
} // namespace Channel
