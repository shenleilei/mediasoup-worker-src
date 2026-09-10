#define MS_CLASS "RTC::IceServer"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/IceServer.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include <exception>

namespace RTC
{
	namespace
	{
		class RemovingTuplesGuard
		{
		public:
			explicit RemovingTuplesGuard(bool& flag) : flag(flag)
			{
				this->flag = true;
			}

			~RemovingTuplesGuard()
			{
				this->flag = false;
			}

			RemovingTuplesGuard(const RemovingTuplesGuard&)            = delete;
			RemovingTuplesGuard& operator=(const RemovingTuplesGuard&) = delete;

		private:
			bool& flag;
		};

		template<typename Callback>
		void InvokeIceListenerNoThrow(const char* context, Callback&& callback) noexcept
		{
			try
			{
				callback();
			}
			catch (const std::exception& error)
			{
				try
				{
					MS_ERROR("%s failed: %s", context, error.what());
				}
				catch (...)
				{
				}
			}
			catch (...)
			{
				try
				{
					MS_ERROR("%s failed with an unknown exception", context);
				}
				catch (...)
				{
				}
			}
		}
	} // namespace

	/* Static. */

	static constexpr size_t StunSerializeBufferSize{ 65536 };
	thread_local static uint8_t StunSerializeBuffer[StunSerializeBufferSize];
	static constexpr size_t MaxTuples{ 8 };
	static constexpr uint8_t ConsentCheckMinTimeoutSec{ 10u };
	static constexpr uint8_t ConsentCheckMaxTimeoutSec{ 60u };
	static constexpr uint64_t ConsentIdleInitialMs{ 6000u };
	static constexpr uint64_t ConsentIdleRepeatMs{ 1000u };

	/* Class methods. */
	IceServer::IceState IceStateFromFbs(FBS::WebRtcTransport::IceState state)
	{
		switch (state)
		{
			case FBS::WebRtcTransport::IceState::NEW:
			{
				return IceServer::IceState::NEW;
			}

			case FBS::WebRtcTransport::IceState::CONNECTED:
			{
				return IceServer::IceState::CONNECTED;
			}

			case FBS::WebRtcTransport::IceState::COMPLETED:
			{
				return IceServer::IceState::COMPLETED;
			}

			case FBS::WebRtcTransport::IceState::DISCONNECTED:
			{
				return IceServer::IceState::DISCONNECTED;
			}
		}
	}

	FBS::WebRtcTransport::IceState IceServer::IceStateToFbs(IceServer::IceState state)
	{
		switch (state)
		{
			case IceServer::IceState::NEW:
			{
				return FBS::WebRtcTransport::IceState::NEW;
			}

			case IceServer::IceState::CONNECTED:
			{
				return FBS::WebRtcTransport::IceState::CONNECTED;
			}

			case IceServer::IceState::COMPLETED:
			{
				return FBS::WebRtcTransport::IceState::COMPLETED;
			}

			case IceServer::IceState::DISCONNECTED:
			{
				return FBS::WebRtcTransport::IceState::DISCONNECTED;
			}
		}
	}

	/* Instance methods. */

	IceServer::IceServer(
	  Listener* listener,
	  const std::string& usernameFragment,
	  const std::string& password,
	  uint8_t consentTimeoutSec)
	  : listener(listener), usernameFragment(usernameFragment), password(password)
	{
		MS_TRACE();

		if (consentTimeoutSec == 0u)
		{
			// 0 means disabled so it's a valid value.
		}
		else if (consentTimeoutSec < ConsentCheckMinTimeoutSec)
		{
			MS_WARN_TAG(
			  ice,
			  "consentTimeoutSec cannot be lower than %" PRIu8 " seconds, fixing it",
			  ConsentCheckMinTimeoutSec);

			consentTimeoutSec = ConsentCheckMinTimeoutSec;
		}
		else if (consentTimeoutSec > ConsentCheckMaxTimeoutSec)
		{
			MS_WARN_TAG(
			  ice,
			  "consentTimeoutSec cannot be higher than %" PRIu8 " seconds, fixing it",
			  ConsentCheckMaxTimeoutSec);

			consentTimeoutSec = ConsentCheckMaxTimeoutSec;
		}

		this->consentTimeoutMs = consentTimeoutSec * 1000;

		// Notify the listener.
		this->listener->OnIceServerLocalUsernameFragmentAdded(this, usernameFragment);
	}

	IceServer::~IceServer() noexcept
	{
		try
		{
			MS_TRACE();
		}
		catch (...)
		{
		}

		// Here we must notify the listener about the removal of current
		// usernameFragments (and also the old one if any) and all tuples.

		InvokeIceListenerNoThrow(
		  "ICE local username removal listener",
		  [this]() { this->listener->OnIceServerLocalUsernameFragmentRemoved(this, usernameFragment); });

		if (!this->oldUsernameFragment.empty())
		{
			InvokeIceListenerNoThrow(
			  "ICE old local username removal listener",
			  [this]()
			  {
				  this->listener->OnIceServerLocalUsernameFragmentRemoved(
				    this, this->oldUsernameFragment);
			  });
		}

		// Clear all tuples.
		{
			RemovingTuplesGuard removingTuplesGuard(this->isRemovingTuples);

			for (const auto& it : this->tuples)
			{
				auto* storedTuple = const_cast<RTC::TransportTuple*>(std::addressof(it));

				// Notify each listener independently so one failure cannot skip the
				// remaining tuples or timer cleanup during this noexcept destructor.
				InvokeIceListenerNoThrow(
				  "ICE tuple removal listener",
				  [this, storedTuple]() { this->listener->OnIceServerTupleRemoved(this, storedTuple); });
			}
		}

		// Clear all tuples.
		// NOTE: Do it after notifying the listener since the listener may need to
		// use/read the tuple being removed so we cannot free it yet.
		this->tuples.clear();

		// Unset selected tuple.
		this->selectedTuple = nullptr;

		// Delete the ICE consent timers.
		delete this->consentCheckTimer;
		this->consentCheckTimer = nullptr;
		delete this->consentIdleTimer;
		this->consentIdleTimer = nullptr;
	}

#ifdef MS_TEST
	void IceServer::StartConsentTimeoutForTesting(RTC::TransportTuple* tuple, uint64_t timeoutMs)
	{
		auto* storedTuple = AddTuple(tuple);
		this->selectedTuple = storedTuple;
		this->state         = IceState::CONNECTED;

		if (!this->consentCheckTimer)
		{
			this->consentCheckTimer = new TimerHandle(this);
		}
		else if (this->consentCheckTimer->IsActive())
		{
			this->consentCheckTimer->Stop();
		}

		this->consentCheckTimer->Start(timeoutMs);
	}

	void IceServer::StartConsentIdleForTesting(
	  RTC::TransportTuple* tuple, uint64_t initialMs, uint64_t repeatMs)
	{
		auto* storedTuple                    = AddTuple(tuple);
		this->selectedTuple                  = storedTuple;
		this->state                          = IceState::CONNECTED;
		this->lastConsentRequestReceivedAtMs = DepLibUV::GetTimeMs();

		if (!this->consentIdleTimer)
		{
			this->consentIdleTimer = new TimerHandle(this);
		}

		this->consentIdleTimer->Start(initialMs, repeatMs);
	}

	void IceServer::ReceiveConsentRequestForTesting()
	{
		HandleConsentRequestReceived();
	}
#endif

	void IceServer::ProcessStunPacket(RTC::StunPacket* packet, RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		switch (packet->GetClass())
		{
			case RTC::StunPacket::Class::REQUEST:
			{
				ProcessStunRequest(packet, tuple);

				break;
			}

			case RTC::StunPacket::Class::INDICATION:
			{
				ProcessStunIndication(packet);

				break;
			}

			case RTC::StunPacket::Class::SUCCESS_RESPONSE:
			case RTC::StunPacket::Class::ERROR_RESPONSE:
			{
				ProcessStunResponse(packet);

				break;
			}

			default:
			{
				MS_WARN_TAG(
				  ice, "unknown STUN class %" PRIu16 ", discarded", static_cast<uint16_t>(packet->GetClass()));
			}
		}
	}

	void IceServer::RestartIce(const std::string& usernameFragment, const std::string& password)
	{
		MS_TRACE();

		if (!this->oldUsernameFragment.empty())
		{
			this->listener->OnIceServerLocalUsernameFragmentRemoved(this, this->oldUsernameFragment);
		}

		this->oldUsernameFragment = this->usernameFragment;
		this->usernameFragment    = usernameFragment;

		this->oldPassword = this->password;
		this->password    = password;

		this->remoteNomination = 0u;

		// Notify the listener.
		this->listener->OnIceServerLocalUsernameFragmentAdded(this, usernameFragment);

		// NOTE: Do not call listener->OnIceServerLocalUsernameFragmentRemoved()
		// yet with old usernameFragment. Wait until we receive a STUN packet
		// with the new one.

		// Restart ICE consent check (if running) to give some time to the
		// client to establish ICE again.
		if (IsConsentCheckSupported())
		{
			if (IsConsentCheckRunning())
			{
				RestartConsentCheck();
			}
			this->consentIdle = false;
			if (this->consentIdleTimer)
			{
				if (this->consentIdleTimer->IsActive())
				{
					RestartConsentIdleCheck();
				}
				else
				{
					StartConsentIdleCheck();
				}
			}
			else
			{
				StartConsentIdleCheck();
			}
		}
	}

	bool IceServer::IsValidTuple(const RTC::TransportTuple* tuple) const
	{
		MS_TRACE();

		return HasTuple(tuple) != nullptr;
	}

	void IceServer::RemoveTuple(RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		// If IceServer is removing a tuple or all tuples (for instance in the
		// destructor), the OnIceServerTupleRemoved() callback may end triggering
		// new calls to RemoveTuple(). We must ignore it to avoid double-free issues.
		if (this->isRemovingTuples)
		{
			return;
		}

		RTC::TransportTuple* removedTuple{ nullptr };

		// Find the removed tuple.
		auto it = this->tuples.begin();

		for (; it != this->tuples.end(); ++it)
		{
			RTC::TransportTuple* storedTuple = std::addressof(*it);

			if (storedTuple->Compare(tuple))
			{
				removedTuple = storedTuple;

				break;
			}
		}

		// If not found, ignore.
		if (!removedTuple)
		{
			return;
		}

		const bool removedSelectedTuple = removedTuple == this->selectedTuple;

		// Notify the listener while suppressing re-entrant removals. Listener
		// failure must neither leave the guard latched nor cancel terminal tuple
		// cleanup, otherwise a closing TCP connection remains referenced forever.
		{
			RemovingTuplesGuard removingTuplesGuard(this->isRemovingTuples);

			try
			{
				this->listener->OnIceServerTupleRemoved(this, removedTuple);
			}
			catch (const std::exception& error)
			{
				try
				{
					MS_ERROR("ICE tuple removal listener failed: %s", error.what());
				}
				catch (...)
				{
				}
			}
			catch (...)
			{
				try
				{
					MS_ERROR("ICE tuple removal listener failed with an unknown exception");
				}
				catch (...)
				{
				}
			}
		}

		// Remove it from the list of tuples.
		// NOTE: Do it after notifying the listener since the listener may need to
		// use/read the tuple being removed so we cannot free it yet.
		this->tuples.erase(it);

		// If this is the selected tuple, do things.
		if (removedSelectedTuple)
		{
			this->selectedTuple = nullptr;

			// Mark the first tuple as selected tuple (if any) but only if state was
			// 'connected' or 'completed'.
			if (
			  (this->state == IceState::CONNECTED || this->state == IceState::COMPLETED) &&
			  this->tuples.begin() != this->tuples.end())
			{
				SetSelectedTuple(std::addressof(*this->tuples.begin()));

				// Restart ICE consent check to let the client send new consent requests
				// on the new selected tuple.
				if (IsConsentCheckSupported())
				{
					RestartConsentCheck();
				}
			}
			// Or just emit 'disconnected'.
			else
			{
				InvokeIceListenerNoThrow(
				  "ICE disconnected transition log",
				  [this]()
				  {
					  MS_WARN_TAG(
					    ice,
					    "transition from state '%s' to 'disconnected' [reason:selected tuple removed]",
					    this->state == IceState::CONNECTED ? "connected" : "completed");
				  });

				// Update state.
				this->state = IceState::DISCONNECTED;

				// Reset remote nomination.
				this->remoteNomination = 0u;

				// Stop terminal timer activity before notifying user code. If the
				// notification allocates and fails, no later timer callback can observe
				// DISCONNECTED with a null selected tuple and abort on its invariants.
				if (IsConsentCheckSupported())
				{
					if (IsConsentCheckRunning())
					{
						StopConsentCheck();
					}
					this->consentIdle = false;
					if (this->consentIdleTimer && this->consentIdleTimer->IsActive())
					{
						StopConsentIdleCheck();
					}
				}

				InvokeIceListenerNoThrow(
				  "ICE disconnected listener",
				  [this]() { this->listener->OnIceServerDisconnected(this); });
			}
		}
	}

	void IceServer::ProcessStunRequest(RTC::StunPacket* request, RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		MS_DEBUG_DEV("processing STUN request");

		// Must be a Binding method.
		if (request->GetMethod() != RTC::StunPacket::Method::BINDING)
		{
			MS_WARN_TAG(
			  ice,
			  "STUN request with unknown method %#.3x => 400",
			  static_cast<unsigned int>(request->GetMethod()));

			// Reply 400.
			RTC::StunPacket* response = request->CreateErrorResponse(400);

			response->Serialize(StunSerializeBuffer);
			this->listener->OnIceServerSendStunPacket(this, response, tuple);

			delete response;

			return;
		}

		// Must have FINGERPRINT attribute.
		if (!request->HasFingerprint())
		{
			MS_WARN_TAG(ice, "STUN Binding request without FINGERPRINT attribute => 400");

			// Reply 400.
			RTC::StunPacket* response = request->CreateErrorResponse(400);

			response->Serialize(StunSerializeBuffer);
			this->listener->OnIceServerSendStunPacket(this, response, tuple);

			delete response;

			return;
		}

		// PRIORITY attribute is required.
		if (request->GetPriority() == 0u)
		{
			MS_WARN_TAG(ice, "STUN Binding request without PRIORITY attribute => 400");

			// Reply 400.
			RTC::StunPacket* response = request->CreateErrorResponse(400);

			response->Serialize(StunSerializeBuffer);
			this->listener->OnIceServerSendStunPacket(this, response, tuple);

			delete response;

			return;
		}

		// Check authentication.
		switch (request->CheckAuthentication(this->usernameFragment, this->password))
		{
			case RTC::StunPacket::Authentication::OK:
			{
				if (!this->oldUsernameFragment.empty() && !this->oldPassword.empty())
				{
					MS_DEBUG_TAG(ice, "new ICE credentials applied");

					// Notify the listener.
					this->listener->OnIceServerLocalUsernameFragmentRemoved(this, this->oldUsernameFragment);

					this->oldUsernameFragment.clear();
					this->oldPassword.clear();
				}

				break;
			}

			case RTC::StunPacket::Authentication::UNAUTHORIZED:
			{
				// We may have changed our usernameFragment and password, so check the
				// old ones.
				// clang-format off
				if (
				  !this->oldUsernameFragment.empty() &&
				  !this->oldPassword.empty() &&
				  request->CheckAuthentication(
				    this->oldUsernameFragment, this->oldPassword
				  ) == RTC::StunPacket::Authentication::OK
				)
				// clang-format on
				{
					MS_DEBUG_TAG(ice, "using old ICE credentials");

					break;
				}

				MS_WARN_TAG(ice, "wrong authentication in STUN Binding request => 401");

				// Reply 401.
				RTC::StunPacket* response = request->CreateErrorResponse(401);

				response->Serialize(StunSerializeBuffer);
				this->listener->OnIceServerSendStunPacket(this, response, tuple);

				delete response;

				return;
			}

			case RTC::StunPacket::Authentication::BAD_MESSAGE:
			{
				MS_WARN_TAG(ice, "cannot check authentication in STUN Binding request => 400");

				// Reply 400.
				RTC::StunPacket* response = request->CreateErrorResponse(400);

				response->Serialize(StunSerializeBuffer);
				this->listener->OnIceServerSendStunPacket(this, response, tuple);

				delete response;

				return;
			}
		}

		// The remote peer must be ICE controlling.
		if (request->GetIceControlled())
		{
			MS_WARN_TAG(ice, "peer indicates ICE-CONTROLLED in STUN Binding request => 487");

			// Reply 487 (Role Conflict).
			RTC::StunPacket* response = request->CreateErrorResponse(487);

			response->Serialize(StunSerializeBuffer);
			this->listener->OnIceServerSendStunPacket(this, response, tuple);

			delete response;

			return;
		}

		MS_DEBUG_DEV(
		  "valid STUN Binding request [priority:%" PRIu32 ", useCandidate:%s]",
		  static_cast<uint32_t>(request->GetPriority()),
		  request->HasUseCandidate() ? "true" : "false");

		// Create a success response.
		RTC::StunPacket* response = request->CreateSuccessResponse();

		// Add XOR-MAPPED-ADDRESS.
		response->SetXorMappedAddress(tuple->GetRemoteAddress());

		// Authenticate the response.
		if (this->oldPassword.empty())
		{
			response->SetPassword(this->password);
		}
		else
		{
			response->SetPassword(this->oldPassword);
		}

		// Send back.
		response->Serialize(StunSerializeBuffer);
		this->listener->OnIceServerSendStunPacket(this, response, tuple);

		delete response;

		uint32_t nomination{ 0u };

		if (request->HasNomination())
		{
			nomination = request->GetNomination();
		}

		// Handle the tuple.
		HandleTuple(tuple, request->HasUseCandidate(), request->HasNomination(), nomination);

		// If state is 'connected' or 'completed' after handling the tuple, then
		// start or restart ICE consent check (if supported).
		if (IsConsentCheckSupported() && (this->state == IceState::CONNECTED || this->state == IceState::COMPLETED))
		{
			HandleConsentRequestReceived();
		}
	}

	void IceServer::ProcessStunIndication(RTC::StunPacket* indication)
	{
		MS_TRACE();

		MS_DEBUG_DEV("STUN indication received, discarded");

		// Nothig else to do. We just discard STUN indications.
	}

	void IceServer::ProcessStunResponse(RTC::StunPacket* response)
	{
		MS_TRACE();

		const std::string responseType = response->GetClass() == RTC::StunPacket::Class::SUCCESS_RESPONSE
		                                   ? "success"
		                                   : std::to_string(response->GetErrorCode()) + " error";

		MS_DEBUG_DEV("processing STUN %s response received, discarded", responseType.c_str());

		// Nothig else to do. We just discard STUN responses because we do not
		// generate STUN requests.
	}

	void IceServer::MayForceSelectedTuple(const RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		if (this->state != IceState::CONNECTED && this->state != IceState::COMPLETED)
		{
			MS_WARN_TAG(ice, "cannot force selected tuple if not in state 'connected' or 'completed'");

			return;
		}

		auto* storedTuple = HasTuple(tuple);

		if (!storedTuple)
		{
			MS_WARN_TAG(ice, "cannot force selected tuple if the given tuple was not already a valid one");

			return;
		}

		SetSelectedTuple(storedTuple);
	}

	void IceServer::HandleTuple(
	  RTC::TransportTuple* tuple, bool hasUseCandidate, bool hasNomination, uint32_t nomination)
	{
		MS_TRACE();

		switch (this->state)
		{
			case IceState::NEW:
			{
				// There shouldn't be a selected tuple.
				MS_ASSERT(!this->selectedTuple, "state is 'new' but there is selected tuple");

				if (!hasUseCandidate && !hasNomination)
				{
					MS_WARN_TAG(
					  ice,
					  "transition from state 'new' to 'connected' [hasUseCandidate:%s, hasNomination:%s, nomination:%" PRIu32
					  "]",
					  hasUseCandidate ? "true" : "false",
					  hasNomination ? "true" : "false",
					  nomination);

					// Store the tuple.
					auto* storedTuple = AddTuple(tuple);

					// Update state.
					this->state = IceState::CONNECTED;

					// Mark it as selected tuple.
					SetSelectedTuple(storedTuple);

					// Notify the listener.
					this->listener->OnIceServerConnected(this);
				}
				else
				{
					// Store the tuple.
					auto* storedTuple = AddTuple(tuple);

					if ((hasNomination && nomination > this->remoteNomination) || !hasNomination)
					{
						MS_WARN_TAG(
						  ice,
						  "transition from state 'new' to 'completed' [hasUseCandidate:%s, hasNomination:%s, nomination:%" PRIu32
						  "]",
						  hasUseCandidate ? "true" : "false",
						  hasNomination ? "true" : "false",
						  nomination);

						// Update state.
						this->state = IceState::COMPLETED;

						// Mark it as selected tuple.
						SetSelectedTuple(storedTuple);

						// Update nomination.
						if (hasNomination && nomination > this->remoteNomination)
						{
							this->remoteNomination = nomination;
						}

						// Notify the listener.
						this->listener->OnIceServerCompleted(this);
					}
				}

				break;
			}

			case IceState::DISCONNECTED:
			{
				// There shouldn't be a selected tuple.
				MS_ASSERT(!this->selectedTuple, "state is 'disconnected' but there is selected tuple");

				if (!hasUseCandidate && !hasNomination)
				{
					MS_WARN_TAG(
					  ice,
					  "transition from state 'disconnected' to 'connected' [hasUseCandidate:%s, hasNomination:%s, nomination:%" PRIu32
					  "]",
					  hasUseCandidate ? "true" : "false",
					  hasNomination ? "true" : "false",
					  nomination);

					// Store the tuple.
					auto* storedTuple = AddTuple(tuple);

					// Update state.
					this->state = IceState::CONNECTED;

					// Mark it as selected tuple.
					SetSelectedTuple(storedTuple);

					// Notify the listener.
					this->listener->OnIceServerConnected(this);
				}
				else
				{
					// Store the tuple.
					auto* storedTuple = AddTuple(tuple);

					if ((hasNomination && nomination > this->remoteNomination) || !hasNomination)
					{
						MS_WARN_TAG(
						  ice,
						  "transition from state 'disconnected' to 'completed' [hasUseCandidate:%s, hasNomination:%s, nomination:%" PRIu32
						  "]",
						  hasUseCandidate ? "true" : "false",
						  hasNomination ? "true" : "false",
						  nomination);

						// Update state.
						this->state = IceState::COMPLETED;

						// Mark it as selected tuple.
						SetSelectedTuple(storedTuple);

						// Update nomination.
						if (hasNomination && nomination > this->remoteNomination)
						{
							this->remoteNomination = nomination;
						}

						// Notify the listener.
						this->listener->OnIceServerCompleted(this);
					}
				}

				break;
			}

			case IceState::CONNECTED:
			{
				// There should be some tuples.
				MS_ASSERT(!this->tuples.empty(), "state is 'connected' but there are no tuples");

				// There should be a selected tuple.
				MS_ASSERT(this->selectedTuple, "state is 'connected' but there is not selected tuple");

				if (!hasUseCandidate && !hasNomination)
				{
					// Store the tuple.
					AddTuple(tuple);
				}
				else
				{
					MS_WARN_TAG(
					  ice,
					  "transition from state 'connected' to 'completed' [hasUseCandidate:%s, hasNomination:%s, nomination:%" PRIu32
					  "]",
					  hasUseCandidate ? "true" : "false",
					  hasNomination ? "true" : "false",
					  nomination);

					// Store the tuple.
					auto* storedTuple = AddTuple(tuple);

					if ((hasNomination && nomination > this->remoteNomination) || !hasNomination)
					{
						// Update state.
						this->state = IceState::COMPLETED;

						// Mark it as selected tuple.
						SetSelectedTuple(storedTuple);

						// Update nomination.
						if (hasNomination && nomination > this->remoteNomination)
						{
							this->remoteNomination = nomination;
						}

						// Notify the listener.
						this->listener->OnIceServerCompleted(this);
					}
				}

				break;
			}

			case IceState::COMPLETED:
			{
				// There should be some tuples.
				MS_ASSERT(!this->tuples.empty(), "state is 'completed' but there are no tuples");

				// There should be a selected tuple.
				MS_ASSERT(this->selectedTuple, "state is 'completed' but there is not selected tuple");

				if (!hasUseCandidate && !hasNomination)
				{
					// Store the tuple.
					AddTuple(tuple);
				}
				else
				{
					// Store the tuple.
					auto* storedTuple = AddTuple(tuple);

					if ((hasNomination && nomination > this->remoteNomination) || !hasNomination)
					{
						// Mark it as selected tuple.
						SetSelectedTuple(storedTuple);

						// Update nomination.
						if (hasNomination && nomination > this->remoteNomination)
						{
							this->remoteNomination = nomination;
						}
					}
				}

				break;
			}
		}
	}

	RTC::TransportTuple* IceServer::AddTuple(RTC::TransportTuple* tuple)
	{
		MS_TRACE();

		auto* storedTuple = HasTuple(tuple);

		if (storedTuple)
		{
			MS_DEBUG_DEV("tuple already exists");

			return storedTuple;
		}

		// Add the new tuple at the beginning of the list.
		this->tuples.push_front(*tuple);

		storedTuple = std::addressof(*this->tuples.begin());

		// If it is UDP then we must store the remote address (until now it is
		// just a pointer that will be freed soon).
		if (storedTuple->GetProtocol() == TransportTuple::Protocol::UDP)
		{
			storedTuple->StoreUdpRemoteAddress();
		}

		// Notify the listener.
		this->listener->OnIceServerTupleAdded(this, storedTuple);

		// Don't allow more than MaxTuples.
		if (this->tuples.size() > MaxTuples)
		{
			MS_WARN_TAG(ice, "too too many tuples, removing the oldest non selected one");

			// Find the oldest tuple which is neither the added one nor the selected
			// one (if any), and remove it.
			RTC::TransportTuple* removedTuple{ nullptr };
			auto it = this->tuples.rbegin();

			for (; it != this->tuples.rend(); ++it)
			{
				RTC::TransportTuple* otherStoredTuple = std::addressof(*it);

				if (otherStoredTuple != storedTuple && otherStoredTuple != this->selectedTuple)
				{
					removedTuple = otherStoredTuple;

					break;
				}
			}

			// This should not happen by design.
			MS_ASSERT(removedTuple, "couldn't find any tuple to be removed");

			// Notify the listener while keeping re-entrant removals suppressed. A
			// listener failure must not leave the flag latched or skip the bounded
			// eviction below.
			{
				RemovingTuplesGuard removingTuplesGuard(this->isRemovingTuples);
				InvokeIceListenerNoThrow(
				  "ICE tuple eviction listener",
				  [this, removedTuple]()
				  { this->listener->OnIceServerTupleRemoved(this, removedTuple); });
			}

			// Remove it from the list of tuples.
			// NOTE: Do it after notifying the listener since the listener may need to
			// use/read the tuple being removed so we cannot free it yet.
			// NOTE: This trick is needed since it is a reverse_iterator and
			// erase() requires a iterator, const_iterator or bidirectional_iterator.
			this->tuples.erase(std::next(it).base());
		}

		// Return the address of the inserted tuple.
		return storedTuple;
	}

	RTC::TransportTuple* IceServer::HasTuple(const RTC::TransportTuple* tuple) const
	{
		MS_TRACE();

		// Check the current selected tuple (if any).
		if (this->selectedTuple && this->selectedTuple->Compare(tuple))
		{
			return this->selectedTuple;
		}

		// Otherwise check other stored tuples.
		for (const auto& it : this->tuples)
		{
			auto* storedTuple = const_cast<RTC::TransportTuple*>(std::addressof(it));

			if (storedTuple->Compare(tuple))
			{
				return storedTuple;
			}
		}

		return nullptr;
	}

	void IceServer::SetSelectedTuple(RTC::TransportTuple* storedTuple)
	{
		MS_TRACE();

		// If already the selected tuple do nothing.
		if (storedTuple == this->selectedTuple)
		{
			return;
		}

		this->selectedTuple = storedTuple;

		// Notify the listener.
		InvokeIceListenerNoThrow(
		  "ICE selected tuple listener",
		  [this]() { this->listener->OnIceServerSelectedTuple(this, this->selectedTuple); });
	}

	void IceServer::StartConsentCheck()
	{
		MS_TRACE();

		MS_ASSERT(IsConsentCheckSupported(), "ICE consent check not supported");
		MS_ASSERT(!IsConsentCheckRunning(), "ICE consent check already running");
		MS_ASSERT(this->selectedTuple, "no selected tuple");

		// Create the ICE consent check timer if it doesn't exist.
		if (!this->consentCheckTimer)
		{
			this->consentCheckTimer = new TimerHandle(this);
		}

		this->consentCheckTimer->Start(this->consentTimeoutMs);
	}

	void IceServer::RestartConsentCheck()
	{
		MS_TRACE();

		MS_ASSERT(IsConsentCheckSupported(), "ICE consent check not supported");
		MS_ASSERT(IsConsentCheckRunning(), "ICE consent check not running");
		MS_ASSERT(this->selectedTuple, "no selected tuple");

		this->consentCheckTimer->Restart();
	}

	void IceServer::StopConsentCheck()
	{
		MS_TRACE();

		MS_ASSERT(IsConsentCheckSupported(), "ICE consent check not supported");
		MS_ASSERT(IsConsentCheckRunning(), "ICE consent check not running");

		this->consentCheckTimer->Stop();
	}

	void IceServer::HandleConsentRequestReceived()
	{
		MS_TRACE();

		this->lastConsentRequestReceivedAtMs = DepLibUV::GetTimeMs();

		if (this->consentIdle)
		{
			this->consentIdle = false;
			InvokeIceListenerNoThrow(
			  "ICE consent active listener",
			  [this]() { this->listener->OnIceServerConsentChange(this, true, 0u); });
		}

		if (IsConsentCheckRunning())
		{
			RestartConsentCheck();
		}
		else
		{
			StartConsentCheck();
		}

		if (this->consentIdleTimer && this->consentIdleTimer->IsActive())
		{
			RestartConsentIdleCheck();
		}
		else
		{
			StartConsentIdleCheck();
		}
	}

	void IceServer::StartConsentIdleCheck()
	{
		MS_TRACE();

		MS_ASSERT(IsConsentCheckSupported(), "ICE consent check not supported");

		if (!this->consentIdleTimer)
		{
			this->consentIdleTimer = new TimerHandle(this);
		}

		this->consentIdleTimer->Start(ConsentIdleInitialMs, ConsentIdleRepeatMs);
	}

	void IceServer::RestartConsentIdleCheck()
	{
		MS_TRACE();

		MS_ASSERT(IsConsentCheckSupported(), "ICE consent check not supported");
		MS_ASSERT(this->consentIdleTimer, "ICE consent idle timer not created");

		this->consentIdleTimer->Restart();
	}

	void IceServer::StopConsentIdleCheck()
	{
		MS_TRACE();

		MS_ASSERT(IsConsentCheckSupported(), "ICE consent check not supported");
		MS_ASSERT(this->consentIdleTimer, "ICE consent idle timer not created");

		this->consentIdleTimer->Stop();
	}

	inline void IceServer::OnTimer(TimerHandle* timer) noexcept
	{
		try
		{
			MS_TRACE();
		}
		catch (...)
		{
		}

		if (timer == this->consentIdleTimer)
		{
			MS_ASSERT(IsConsentCheckSupported(), "ICE consent check not supported");

			// The normal ICE state remains connected until the longer consent timer
			// expires. Emit a low-volume idle signal so the parent runtime can combine
			// it with its application-level viewer liveness signal.
			if (this->state == IceState::CONNECTED || this->state == IceState::COMPLETED)
			{
				this->consentIdle     = true;
				const uint64_t idleMs = DepLibUV::GetTimeMs() - this->lastConsentRequestReceivedAtMs;
				InvokeIceListenerNoThrow(
				  "ICE consent idle listener",
				  [this, idleMs]() { this->listener->OnIceServerConsentChange(this, false, idleMs); });
			}
			return;
		}

		if (timer == this->consentCheckTimer)
		{
			MS_ASSERT(IsConsentCheckSupported(), "ICE consent check not supported");

			// State must be 'connected' or 'completed'.
			MS_ASSERT(
			  this->state == IceState::COMPLETED || this->state == IceState::CONNECTED,
			  "ICE consent check timer fired but state is neither 'completed' nor 'connected'");

			// There should be a selected tuple.
			MS_ASSERT(this->selectedTuple, "ICE consent check timer fired but there is not selected tuple");

			InvokeIceListenerNoThrow(
			  "ICE consent expiration log",
			  []()
			  { MS_WARN_TAG(ice, "ICE consent expired due to timeout, moving to 'disconnected' state"); });

			// A test hook can invoke this method while the timer is still active;
			// real one-shot libuv timers are normally inactive before the callback.
			// In either case, commit terminal timer cleanup before notifications.
			if (IsConsentCheckRunning())
			{
				InvokeIceListenerNoThrow(
				  "ICE consent timer stop", [this]() { this->consentCheckTimer->Stop(); });
			}
			this->consentIdle = false;
			if (this->consentIdleTimer && this->consentIdleTimer->IsActive())
			{
				InvokeIceListenerNoThrow(
				  "ICE consent idle timer stop", [this]() { this->consentIdleTimer->Stop(); });
			}

			// Update state.
			this->state = IceState::DISCONNECTED;

			// Reset remote nomination.
			this->remoteNomination = 0u;

			{
				RemovingTuplesGuard removingTuplesGuard(this->isRemovingTuples);

				for (const auto& it : this->tuples)
				{
					auto* storedTuple = const_cast<RTC::TransportTuple*>(std::addressof(it));

					InvokeIceListenerNoThrow(
					  "ICE consent-expiry tuple removal listener",
					  [this, storedTuple]()
					  { this->listener->OnIceServerTupleRemoved(this, storedTuple); });
				}
			}

			// Clear all tuples.
			// NOTE: Do it after notifying the listener since the listener may need to
			// use/read the tuple being removed so we cannot free it yet.
			this->tuples.clear();

			// Unset selected tuple.
			this->selectedTuple = nullptr;

			InvokeIceListenerNoThrow(
			  "ICE consent-expiry disconnected listener",
			  [this]() { this->listener->OnIceServerDisconnected(this); });
		}
	}
} // namespace RTC
