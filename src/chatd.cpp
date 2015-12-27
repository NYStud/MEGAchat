#include "chatd.h"
#include <base/cservices.h>
#include <gcmpp.h>
#include <retryHandler.h>
#include<libws_log.h>
#include <event2/dns.h>
#include "base64.h"

using namespace std;
using namespace promise;

//#define CHATD_LOG_LISTENER_CALLS //DB_CALLS

#define ID_CSTR(id) id.toString().c_str()

#ifdef CHATD_LOG_LISTENER_CALLS
    #define CHATD_LOG_LISTENER_CALL(fmtString,...) CHATD_LOG_DEBUG("%s: " fmtString, mChatId.toString().c_str(), ##__VA_ARGS__)
#else
    #define CHATD_LOG_LISTENER_CALL(...)
#endif

#ifdef CHATD_LOG_DB_CALLS
    #define CHATD_LOG_DB_CALL(fmtString,...) CHATD_LOG_DEBUG(fmtString, ##__VA_ARGS__)
#else
    #define CHATD_LOG_DB_CALL(...)
#endif

#define CALL_LISTENER(methodName,...)                                                           \
    do {                                                                                        \
      try {                                                                                     \
          CHATD_LOG_LISTENER_CALL("calling " #methodName "()");                               \
          mListener->methodName(__VA_ARGS__);                                                   \
      } catch(std::exception& e) {                                                              \
          CHATD_LOG_WARNING("Exception thrown from Listener::" #methodName "() app handler:\n%s", e.what());\
      }                                                                                         \
    } while(0)

#define CALL_DB(methodName,...)                                                           \
    do {                                                                                        \
      try {                                                                                     \
          CHATD_LOG_DB_CALL("calling " #methodName "()");                               \
          mDbInterface->methodName(__VA_ARGS__);                                                   \
      } catch(std::exception& e) {                                                              \
          CHATD_LOG_WARNING("Exception thrown from DbInterface::" #methodName "():\n%s", e.what());\
      }                                                                                         \
    } while(0)

namespace chatd
{

// message storage subsystem
// the message buffer can grow in two directions and is always contiguous, i.e. there are no "holes"
// there is no guarantee as to ordering

ws_base_s Client::sWebsocketContext;
bool Client::sWebsockCtxInitialized = false;

Client::Client(const Id& userId)
:mUserId(userId)
{
    if (!sWebsockCtxInitialized)
    {
        ws_global_init(&sWebsocketContext, services_get_event_loop(), services_dns_eventbase,
        [](struct bufferevent* bev, void* userp)
        {
            ::mega::marshallCall([bev, userp]()
            {
                //CHATD_LOG_DEBUG("Read event");
                ws_read_callback(bev, userp);
            });
        },
        [](struct bufferevent* bev, short events, void* userp)
        {
            ::mega::marshallCall([bev, events, userp]()
            {
                //CHATD_LOG_DEBUG("Buffer event 0x%x", events);
                ws_event_callback(bev, events, userp);
            });
        },
        [](int fd, short events, void* userp)
        {
            ::mega::marshallCall([events, userp]()
            {
                //CHATD_LOG_DEBUG("Timer %p event", userp);
                ws_handle_marshall_timer_cb(0, events, userp);
            });
        });
//        ws_set_log_cb(ws_default_log_cb);
//        ws_set_log_level(LIBWS_TRACE);
        sWebsockCtxInitialized = true;
    }
    /// random starting point for the new message transaction ID
    /// FIXME: use cryptographically strong PRNG instead
    /// CHECK: is this sufficiently collision-proof? a collision would have to occur in the same second for the same userid.
//    for (int i = 0; i < 8; i++)
//    {
//        static_cast<uint8_t*>(&mMsgTransactionId.val)[i] = rand() & 0xFF;
//    }
}

#define checkLibwsCall(call, opname) \
    do {                             \
        int _cls_ret = (call);       \
        if (_cls_ret) throw std::runtime_error("Websocket error " +std::to_string(_cls_ret) + \
        " on operation " #opname);   \
    } while(0)

//Stale event from a previous connect attempt?
#define ASSERT_NOT_ANOTHER_WS(event)    \
    if (ws != self->mWebSocket) {       \
        CHATD_LOG_WARNING("Websocket '" event "' callback: ws param is not equal to self->mWebSocket, ignoring"); \
    }

// add a new chatd shard
void Client::join(const Id& chatid, int shardNo, const std::string& url, Listener& listener)
{
    auto msgsit = mMessagesForChatId.find(chatid);
    if (msgsit != mMessagesForChatId.end())
    {
        return;
    }

    // instantiate a Connection object for this shard if needed
    Connection* conn;
    bool isNew;
    auto it = mConnections.find(shardNo);
    if (it == mConnections.end())
    {
        isNew = true;
        conn = new Connection(*this, shardNo);
        mConnections.emplace(std::piecewise_construct, std::forward_as_tuple(shardNo),
                             std::forward_as_tuple(conn));
    }
    else
    {
        isNew = false;
        conn = it->second.get();
    }

    conn->mUrl.parse(url);
    // map chatid to this shard
    mConnectionForChatId[chatid] = conn;

    // add chatid to the connection's chatids
    conn->mChatIds.insert(chatid);
    // always update the URL to give the API an opportunity to migrate chat shards between hosts
    Messages* msgs = new Messages(*conn, chatid, listener);
    mMessagesForChatId.emplace(std::piecewise_construct, std::forward_as_tuple(chatid),
                               std::forward_as_tuple(msgs));

    // attempt a connection ONLY if this is a new shard.
    if(isNew)
    {
        conn->reconnect();
    }
    else if (conn->isOnline())
    {
        msgs->join();
        msgs->range();
    }
}

void Connection::websockConnectCb(ws_t ws, void* arg)
{
    Connection* self = static_cast<Connection*>(arg);
    ASSERT_NOT_ANOTHER_WS("connect");
    CHATD_LOG_DEBUG("Chatd connected");

    assert(self->mConnectPromise);
    self->mConnectPromise->resolve();
}

void Url::parse(const std::string& url)
{
    protocol.clear();
    port = 0;
    host.clear();
    path.clear();
    size_t ss = url.find("://");
    if (ss != std::string::npos)
    {
        protocol = url.substr(0, ss);
        std::transform(protocol.begin(), protocol.end(), protocol.begin(), ::tolower);
        ss += 3;
    }
    else
    {
        ss = 0;
        protocol = "http";
    }
    char last = protocol[protocol.size()-1];
    isSecure = (last == 's');

    size_t i = ss;
    for (; i<url.size(); i++)
    {
        char ch = url[i];
        if (ch == ':') //we have port
        {
            size_t ps = i+1;
            host = url.substr(ss, i-ss);
            for (; i<url.size(); i++)
            {
                ch = url[i];
                if ((ch == '/') || (ch == '?'))
                {
                    break;
                }
            }
            port = std::stol(url.substr(ps, i-ps));
            break;
        }
        else if ((ch == '/') || (ch == '?'))
            break;
    }

    host = url.substr(ss, i-ss);

    if (i < url.size()) //not only host and protocol
    {
        //i now points to '/' or '?' and host and port must have been set
        path = (url[i] == '/') ? url.substr(i+1) : url.substr(i); //ignore the leading '/'
    }
    if (!port)
    {
        port = getPortFromProtocol();
    }
    assert(!host.empty());
}

uint16_t Url::getPortFromProtocol() const
{
    if ((protocol == "http") || (protocol == "ws"))
        return 80;
    else if ((protocol == "https") || (protocol == "wss"))
        return 443;
    else
        return 0;
}
void Connection::websockCloseCb(ws_t ws, int errcode, int errtype, const char *reason,
                                size_t reason_len, void *arg)
{
    auto self = static_cast<Connection*>(arg);
    ASSERT_NOT_ANOTHER_WS("close/error");
    try
    {
        CHATD_LOG_DEBUG("Socket close on connection to shard %d. Reason: %.*s",
                        self->mShardNo, reason_len, reason);
        if (errtype == WS_ERRTYPE_DNS)
        {
            CHATD_LOG_DEBUG("DNS error: forcing libevent to re-read /etc/resolv.conf");
            //if we didn't have our network interface up at app startup, and resolv.conf is
            //genereated dynamically, dns may never work unless we re-read the resolv.conf file
#ifndef _WIN32
            evdns_base_clear_host_addresses(services_dns_eventbase);
            evdns_base_resolv_conf_parse(services_dns_eventbase,
                DNS_OPTIONS_ALL & (~DNS_OPTION_SEARCH), "/etc/resolv.conf");
#else
        evdns_config_windows_nameservers(services_dns_eventbase);
#endif
        }
        ::mega::marshallCall([self]()
        {
            //we don't want to initiate websocket reconnect from within a websocket callback
            self->onSocketClose();
        });
    }
    catch(std::exception& e)
    {
        CHATD_LOG_ERROR("Exception in websocket close callback: %s", e.what());
    }
}

void Connection::onSocketClose()
{
    if (mPingTimer)
    {
        ::mega::cancelInterval(mPingTimer);
        mPingTimer = 0;
    }
    for (auto& chatid: mChatIds)
    {
        mClient.chatidMessages(chatid).setOnlineState(kChatStateOffline);
    }

    auto pms = mConnectPromise.get();
    if (pms && !pms->done())
    {
        pms->reject(getState(), 0x3e9a4a1d);
    }
    pms = mDisconnectPromise.get();
    if (pms && !pms->done())
    {
        pms->resolve();
    }
    if (!mTerminating)
    {
        disconnect();
        reconnect();
    }
}

Promise<void> Connection::reconnect()
{
    int state = getState();
    if ((state == WS_STATE_CONNECTING) || (state == WS_STATE_CONNECTED))
    {
        throw std::runtime_error("Connection::reconnect: Already connected/connecting");
    }
    return
    ::mega::retry([this](int no)
    {
        reset();
        CHATD_LOG_DEBUG("Chatd connecting to shard %d...", mShardNo);
        checkLibwsCall((ws_init(&mWebSocket, &Client::sWebsocketContext)), "create socket");
        ws_set_onconnect_cb(mWebSocket, &websockConnectCb, this);
        ws_set_onclose_cb(mWebSocket, &websockCloseCb, this);
        ws_set_onmsg_cb(mWebSocket,
            [](ws_t ws, char *msg, uint64_t len, int binary, void *arg)
            {
                Connection* self = static_cast<Connection*>(arg);
                ASSERT_NOT_ANOTHER_WS("message");
                self->execCommand(StaticBuffer(msg, len));
            }, this);

        mConnectPromise.reset(new Promise<void>);
        if (mUrl.isSecure)
        {
             ws_set_ssl_state(mWebSocket, LIBWS_SSL_SELFSIGNED);
        }
        for (auto& chatid: mChatIds)
        {
            mClient.chatidMessages(chatid).setOnlineState(kChatStateConnecting);
        }
        checkLibwsCall((ws_connect(mWebSocket, mUrl.host.c_str(), mUrl.port, (mUrl.path).c_str())), "connect");
        return *mConnectPromise;
    }, nullptr, 0, 0, KARERE_RECONNECT_DELAY_MAX, KARERE_RECONNECT_DELAY_INITIAL)
    .then([this]()
    {
        rejoinExistingChats();
        resendPending();
        if (!mPingTimer)
        {
            mPingTimer = ::mega::setInterval([this]()
            {
                if (!isOnline())
                    return;
                sendCommand(Command(OP_KEEPALIVE));
            }, mClient.pingIntervalSec*1000);
        }
    });
}

Promise<void> Connection::disconnect()
{
    if (!mWebSocket)
    {
        return promise::_Void();
    }
    if (!mDisconnectPromise)
    {
        mDisconnectPromise.reset(new Promise<void>);
    }
    ws_close(mWebSocket);
    return *mDisconnectPromise;
}

void Connection::reset()
{
    if (!mWebSocket)
    {
        return;
    }
    ws_close_immediately(mWebSocket);
    ws_destroy(&mWebSocket);
    assert(!mWebSocket);
}

bool Connection::sendCommand(Command&& cmd)
{
    if (!isOnline())
        return false;
//WARNING: ws_send_msg_ex() is destructive to the buffer - it applies the websocket mask directly
    auto rc = ws_send_msg_ex(mWebSocket, cmd.buf(), cmd.dataSize(), 1);
    bool result = (!rc && isOnline());
    cmd.free(); //just in case, as it's content is xor-ed with the websock datamask so it's unusable
    return result;
}

// rejoin all open chats after reconnection (this is mandatory)
void Connection::rejoinExistingChats()
{
    for (auto& chatid: mChatIds)
    {
        // rejoin chat and immediately set the locally buffered message range
        Messages& msgs = mClient.chatidMessages(chatid);
        msgs.join();
        msgs.range();
    }
}

// resend all unconfirmed messages (this is mandatory)
void Connection::resendPending()
{
    for (auto& chatid: mChatIds)
    {
        mClient.chatidMessages(chatid).resendPending();
    }
}

bool Messages::sendCommand(Command&& cmd)
{
    auto opcode = cmd.opcode();
    bool ret = mConnection.sendCommand(std::move(cmd));
    if (ret)
        CHATD_LOG_DEBUG("%s: send %s", ID_CSTR(mChatId), Command::opcodeToStr(opcode));
    else
        CHATD_LOG_DEBUG("%s: Can't send %s, we are offline", ID_CSTR(mChatId), Command::opcodeToStr(opcode));
    return ret;
}

// send JOIN
void Messages::join()
{
    setOnlineState(kChatStateJoining);
    sendCommand(Command(OP_JOIN) + mChatId + mClient.mUserId + (int8_t)PRIV_NOCHANGE);
}

bool Messages::getHistory(int count)
{
    if (mOldestKnownMsgId)
    {
        getHistoryFromDb(count);
        return false;
    }
    else
    {
        requestHistoryFromServer(-count);
        return true;
    }
}
void Messages::requestHistoryFromServer(int32_t count)
{
        mLastHistFetchCount = 0;
        mHistFetchState = kHistFetchingFromServer;
        sendCommand(Command(OP_HIST) + mChatId + count);
}

Messages::Messages(Connection& conn, const Id& chatid, Listener& listener)
    : mConnection(conn), mClient(conn.mClient), mChatId(chatid), mListener(&listener)
{
    Idx newestDbIdx;
    //we don't use CALL_LISTENER here because if init() throws, then something is wrong and we should not continue
    mListener->init(this, mDbInterface);
    assert(mDbInterface);
    mDbInterface->getHistoryInfo(mOldestKnownMsgId, mNewestKnownMsgId, newestDbIdx);
    if (!mOldestKnownMsgId)
    {
        CHATD_LOG_WARNING("%s: Db has no local history for chat", ID_CSTR(mChatId));
        mForwardStart = CHATD_IDX_RANGE_MIDDLE;
        mNewestKnownMsgId = Id::null();
    }
    else
    {
        assert(mNewestKnownMsgId); assert(newestDbIdx != CHATD_IDX_INVALID);
        mForwardStart = newestDbIdx + 1;
        CHATD_LOG_DEBUG("Db has local history for chat %s: %s - %s (middle point: %u)",
            ID_CSTR(mChatId), ID_CSTR(mOldestKnownMsgId), ID_CSTR(mNewestKnownMsgId), mForwardStart);
        getHistoryFromDb(1); //to know if we have the latest message on server, we must at least load the latest db message
    }
}
Messages::~Messages()
{
    clear();
    delete mDbInterface;
}

void Messages::getHistoryFromDb(unsigned count)
{
    assert(mOldestKnownMsgId);
    mHistFetchState = kHistFetchingFromDb;
    std::vector<Message*> messages;
    CALL_DB(fetchDbHistory, lownum()-1, count, messages);
    mLastHistFetchCount = 0;
    for (auto msg: messages)
    {
        msgIncoming(false, msg, true); //increments mLastHistFetchCount
    }
    mHistFetchState = kHistNotFetching;
    CALL_LISTENER(onHistoryDone, true);
    if ((messages.size() < count) && mOldestKnownMsgId)
        throw std::runtime_error("Application says no more messages in db, but we still haven't seen specified oldest message id");
}

//These throw ParseError and will cause the loop to abort.
//These are critical errors that are not per-command

#define READ_ID(varname, offset)\
    assert(offset==pos-base); Id varname(buf.read<uint64_t>(pos)); pos+=sizeof(uint64_t)
#define READ_32(varname, offset)\
    assert(offset==pos-base); uint32_t varname(buf.read<uint32_t>(pos)); pos+= sizeof(uint32_t)

// inbound command processing
// multiple commands can appear as one WebSocket frame, but commands never cross frame boundaries
// CHECK: is this assumption correct on all browsers and under all circumstances?
void Connection::execCommand(const StaticBuffer &buf)
{
    size_t pos = 0;
//IMPORTANT: Increment pos before calling the command handler, because the handler may throw, in which
//case the next iteration will not advance and will execute the same command again, resulting in
//infinite loop
    while (pos < buf.dataSize())
    {
      char opcode = buf.buf()[pos];
      try
      {
        pos++;
#ifndef _NDEBUG
        size_t base = pos;
#endif
//        CHATD_LOG_DEBUG("RECV %s", Command::opcodeToStr(opcode));
        switch (opcode)
        {
            case OP_KEEPALIVE:
            {
                //CHATD_LOG_DEBUG("Server heartbeat received");
                sendCommand(Command(OP_KEEPALIVE));
                break;
            }
            case OP_JOIN:
            {
                READ_ID(chatid, 0);
                READ_ID(userid, 8);
                char priv = buf.read<int8_t>(pos);
                pos++;
                CHATD_LOG_DEBUG("%s: recv JOIN - user '%s' with privilege level %d",
                                ID_CSTR(chatid), ID_CSTR(userid), priv);
                auto& msgs =  mClient.chatidMessages(chatid);
                msgs.onUserJoin(userid, priv);
                break;
            }
            case OP_OLDMSG:
            case OP_NEWMSG:
            {
                bool isNewMsg = (opcode == OP_NEWMSG);
                READ_ID(chatid, 0);
                READ_ID(userid, 8);
                READ_ID(msgid, 16);
                READ_32(ts, 24);
                READ_32(msglen, 28);
                const char* msg = buf.read(pos, msglen);
                pos += msglen;
                CHATD_LOG_DEBUG("%s: recv %s: '%s', from user '%s' with len %u",
                    ID_CSTR(chatid), (isNewMsg ? "NEWMSG" : "OLDMSG"), ID_CSTR(msgid),
                    ID_CSTR(userid), msglen);

                mClient.chatidMessages(chatid).msgIncoming(
                    isNewMsg, new Message(msgid, userid, ts, msg, msglen, nullptr), false);
                break;
            }
            case OP_SEEN:
            {
            //TODO: why do we test the whole buffer's len to determine the current command's len?
            //buffer may contain other commands following it
                READ_ID(chatid, 0);
                READ_ID(msgid, 8);
                CHATD_LOG_DEBUG("%s: recv SEEN - msgid: '%s'",
                                ID_CSTR(chatid), ID_CSTR(msgid));
                mClient.chatidMessages(chatid).onLastSeen(msgid);
                break;
            }
            case OP_RECEIVED:
            {
                READ_ID(chatid, 0);
                READ_ID(msgid, 8);
                CHATD_LOG_DEBUG("%s: recv RECEIVED - msgid: '%s'", ID_CSTR(chatid), ID_CSTR(msgid));
                mClient.chatidMessages(chatid).onLastReceived(msgid);
                break;
            }
            case OP_RETENTION:
            {
                READ_ID(chatid, 0);
                READ_ID(userid, 8);
                READ_32(period, 16);
                CHATD_LOG_DEBUG("%s: recv RETENTION by user '%s' to %u second(s)",
                                ID_CSTR(chatid), ID_CSTR(userid), period);
                break;
            }
            case OP_MSGID:
            {
                READ_ID(msgxid, 0);
                READ_ID(msgid, 8);
                CHATD_LOG_DEBUG("recv MSGID: '%s' -> '%s'", ID_CSTR(msgxid), ID_CSTR(msgid));
                mClient.msgConfirm(msgxid, msgid);
                break;
            }
            case OP_RANGE:
            {
                READ_ID(chatid, 0);
                READ_ID(oldest, 8);
                READ_ID(newest, 16);
                CHATD_LOG_DEBUG("%s: recv RANGE: %s - %s",
                                ID_CSTR(chatid), ID_CSTR(oldest), ID_CSTR(newest));
                auto& msgs = mClient.chatidMessages(chatid);
                if (msgs.onlineState() == kChatStateJoining)
                    msgs.initialFetchHistory(newest);
                break;
            }
            case OP_REJECT:
            {
                READ_ID(id, 0);
                READ_32(op, 8);
                READ_32(code, 12); //TODO: what's this code?
                CHATD_LOG_DEBUG("recv REJECT: id='%s', %d / %d", ID_CSTR(id), op, code);
                if (op == OP_NEWMSG)  // the message was rejected
                {
                    mClient.msgConfirm(id, Id::null());
                }
                else
                {
                    CHATD_LOG_WARNING("%s rejected", Command::opcodeToStr(op));
                }
                break;
            }
            case OP_HISTDONE:
            {
                READ_ID(chatid, 0);
                CHATD_LOG_DEBUG("%s: recv HISTDONE: history retrieval finished", ID_CSTR(chatid));
                mClient.chatidMessages(chatid).onHistDone();
                break;
            }
            default:
                CHATD_LOG_ERROR("Unknown opcode %d, ignoring all subsequent comment", opcode);
                return;
        }
      }
      catch(BufferRangeError& e)
      {
            CHATD_LOG_ERROR("Buffer bound check error while parsing %s: %s\nAborting command processing", Command::opcodeToStr(opcode), e.what());
            return;
      }
      catch(std::exception& e)
      {
            CHATD_LOG_ERROR("Exception while processing incoming %s: %s", Command::opcodeToStr(opcode), e.what());
      }
    }
}

void Messages::msgSend(const Message& message)
{
    Buffer encrypted;
    CALL_LISTENER(encryptMessage, message, encrypted);
    sendCommand(Command(OP_NEWMSG) + mChatId + Id::null() + message.id() + message.ts + encrypted);
}

void Messages::onHistDone()
{
    assert(mHistFetchState == kHistFetchingFromServer);
    mHistFetchState = (mLastHistFetchCount > 0) ? kHistNotFetching : kHistNoMore;
    mListener->onHistoryDone(false);
    if(mOnlineState == kChatStateJoining)
    {
        //only on the first HISTDONE - we may have other history requests afterwards
        onJoinComplete();
    }
}

void Messages::loadAndProcessUnsent()
{
    std::vector<Message*> messages;
    CALL_DB(loadSendingTable, messages);
    for (auto msg: messages)
    {
        if (msg->edits())
        {
            std::unique_ptr<Message> autodel(msg); //in case msgModify() throws
            msgModify(msg->edits(), msg->editsIsXid(), msg->buf(), msg->dataSize(), nullptr, msg->id());
        }
        else
        {
            doMsgSubmit(msg);
        }
        CALL_LISTENER(onUnsentMsgLoaded, *msg);
    }
}

Message* Messages::msgSubmit(const char* msg, size_t msglen, void* userp)
{
    // write the new message to the message buffer and mark as in sending state
    Message* message = new Message(Id::null(), mConnection.mClient.mUserId, time(NULL), msg, msglen, userp, true);
    assert(message->isSending());
    doMsgSubmit(message);
    return message;
}

void Messages::doMsgSubmit(Message* message)
{
    if (!message->id())
        CALL_DB(saveMsgToSending, *message); //assigns a proper msgxid
    else
        assert(message->isSending());

    auto ret = mSending.insert(std::make_pair(message->id(), message));
    if (!ret.second)
        throw std::runtime_error("doMsgSubmit: provided msgxid is not unique");

    //if we believe to be online, send immediately
    //we don't sent a msgStatusChange event to the listener, as the GUI should initialize the
    //message's status with something already, so it's redundant.
    //The GUI should by default show it as sending
    if (mConnection.isOnline())
    {
        msgSend(*message);
    }
}

Message* Messages::msgModify(const Id& oriId, bool editsXid, const char* msg, size_t msglen, void* userp, const Id &ownId)
{
    auto it = mIdToIndexMap.find(oriId);
    if (!editsXid)
    {
        if (it != mIdToIndexMap.end())
        {
            auto& original = at(it->second);
            assert(!original.isSending()); //should never be, as it's in the buffer
            if (original.edits())
                throw std::runtime_error("msgModify: Can't edit an edit message");
        }
        auto editMsg = new Message(Id::null(), mConnection.mClient.userId(), time(NULL), msg, msglen, userp, true);
        editMsg->setEdits(oriId, false); //oriId is a real msgid
        doMsgSubmit(editMsg);
        return editMsg;
    }
    //oriId is a msgxid, must be in mSending
    auto sendIt = mSending.find(oriId);
    if (sendIt == mSending.end()) //It is guaranteed that the original message has already been added to mSent, as the db returns in creation order
        throw std::runtime_error("msgModify: Edited msgxid not found in mSending");
    auto currEdit = sendIt->second.edit;
    if (currEdit) //if we already have a pending edit message, delete it!
    {
        CHATD_LOG_DEBUG("Replacing edit of an unsent msg");
        currEdit->assign(msg, msglen); //impersonate the previous edit, and update it in the db
        currEdit->ts = time(NULL);
        CALL_DB(updateMsgInSending, *currEdit);
    }
    else
    {
        currEdit = sendIt->second.edit = new Message(ownId, mConnection.mClient.mUserId, time(NULL), msg, msglen, userp, true);
        if (!ownId) //otherwise we have just loaded it from db and have a msgxid = rowid
        {
            printf("ownId = %lld\n", ownId.val);
            CALL_DB(saveMsgToSending, *currEdit);
        }
    }
    return currEdit;
}

void Messages::onLastReceived(const Id& msgid)
{
    mLastReceivedId = msgid;
    auto it = mIdToIndexMap.find(msgid);
    if (it == mIdToIndexMap.end())
    { // we don't have that message in the buffer yet, so we don't know its index
        Idx idx = mDbInterface->getIdxOfMsgid(msgid);
        if (idx != CHATD_IDX_INVALID)
        {
            if ((mLastReceivedIdx != CHATD_IDX_INVALID) && (idx < mLastReceivedIdx))
            {
                CHATD_LOG_ERROR("onLastReceived: Tried to set the index to an older message, ignoring");
                CHATD_LOG_DEBUG("highnum() = %zu, mLastReceivedIdx = %zu, idx = %zu", highnum(), mLastReceivedIdx, idx);
            }
            else
            {
                mLastReceivedIdx = idx;
            }
        }
        return; //last-received is behind history in memory, so nothing to notify about
    }

    auto idx = it->second;
    if (idx == mLastReceivedIdx)
        return; //probably set from db
    if (at(idx).userid != mClient.mUserId)
    {
        CHATD_LOG_WARNING("%s: Last-received pointer points to a message by a peer,"
            " possibly the pointer was set incorrectly", ID_CSTR(mChatId));
    }
    //notify about messages that become 'received'
    Idx notifyOldest;
    if (mLastReceivedIdx != CHATD_IDX_INVALID) //we have a previous last-received index, notify user about received messages
    {
        if (mLastReceivedIdx > idx)
        {
            CHATD_LOG_ERROR("onLastReceived: Tried to set the index to an older message, ignoring");
            CHATD_LOG_DEBUG("highnum() = %zu, mLastReceivedIdx = %zu, idx = %zu", highnum(), mLastReceivedIdx, idx);
            return;
        }
        notifyOldest = mLastReceivedIdx;
        mLastReceivedIdx = idx;
    } //no mLastReceivedIdx
    else
    {
        mLastReceivedIdx = idx;
        notifyOldest = lownum();
    }
    for (Idx i=notifyOldest; i<=mLastReceivedIdx; i++)
    {
        auto& msg = at(i);
        if (msg.userid == mClient.mUserId)
        {
            mListener->onMessageStatusChange(i, Message::kDelivered, msg);
        }
    }
}

void Messages::onLastSeen(const Id& msgid)
{
    mLastSeenId = msgid;
    auto it = mIdToIndexMap.find(msgid);
    if (it == mIdToIndexMap.end())
    {
        Idx idx = mDbInterface->getIdxOfMsgid(msgid);
        //last seen is older than our history, so all history in memory is 'unseen', even if there was a previous, older last-seen
        if (idx != CHATD_IDX_INVALID)
        {
            if ((mLastSeenIdx != CHATD_IDX_INVALID) && (idx < mLastSeenIdx))
            {
                CHATD_LOG_ERROR("onLastSeen: Can't set last seen index to an older message");
            }
            else
            {
                mLastSeenIdx = idx;
            }
        }
        return;
    }

    auto idx = it->second;
    if (idx == mLastSeenIdx)
        return; //we may have set it from db already
    if(at(idx).userid == mClient.mUserId)
    {
        CHATD_LOG_WARNING("Last-seen points to a message by us, possibly the pointer was not set properly");
    }
    //notify about messages that have become 'seen'
    Idx notifyOldest;
    if (mLastSeenIdx != CHATD_IDX_INVALID)
    {
        if (idx < mLastSeenIdx)
            throw std::runtime_error("onLastSeen: Can't set last seen index to an older message");
        notifyOldest = mLastSeenIdx;
        mLastSeenIdx = idx;
    }
    else
    {
        mLastSeenIdx = idx;
        notifyOldest = lownum();
    }
    for (Idx i=notifyOldest; i<=mLastSeenIdx; i++)
    {
        auto& msg = at(i);
        if (msg.userid != mClient.mUserId)
        {
            mListener->onMessageStatusChange(i, Message::kSeen, msg);
        }
    }
}
bool Messages::setMessageSeen(Idx idx)
{
    assert(idx != CHATD_IDX_INVALID);
    if (idx <= mLastSeenIdx)
    {
        CHATD_LOG_DEBUG("Attempted to move the last-seen pointer backward, ignoring");
        return false;
    }
    auto& msg = at(idx);
    if (msg.userid == mClient.mUserId)
    {
        CHATD_LOG_DEBUG("Asked to mark own message %s as seen, ignoring", ID_CSTR(msg.id()));
        return false;
    }
    sendCommand(Command(OP_SEEN) + mChatId + msg.id());
    for (Idx i=mLastSeenIdx; i<=idx; i++)
    {
        auto& m = at(i);
        if (m.userid != mClient.mUserId)
        {
            mListener->onMessageStatusChange(i, Message::kSeen, m);
        }
    }
    return true;
}

int Messages::unreadMsgCount() const
{
    Idx first;
    bool mayHaveMore;
    if (mLastSeenIdx == CHATD_IDX_INVALID)
    {
        first = lownum();
        mayHaveMore = true;
    }
    else if (mLastSeenIdx < lownum())
    {
        return mDbInterface->getPeerMsgCountAfterIdx(mLastSeenIdx);
    }
    else
    {
        first = mLastSeenIdx;
        mayHaveMore = false;
    }
    unsigned count = 0;
    auto last = highnum();
    for (Idx i=first; i<=last; i++)
    {
        auto& msg = at(i);
        if ((msg.userid != mClient.userId()) && !msg.edits()) //FIXME: This is not accurate - an edit of a seen message will not be counted
            count++;
    }
    return mayHaveMore?-count:count;
}

void Messages::resendPending()
{
    // resend all pending new messages
    for (auto& item: mSending)
    {
        assert(item.second.msg->isSending());
        msgSend(*item.second.msg);
    }
}

// after a reconnect, we tell the chatd the oldest and newest buffered message
void Messages::range()
{
    if (mOldestKnownMsgId)
    {
        CHATD_LOG_DEBUG("%s: Sending RANGE based on app db: %s - %s", ID_CSTR(mChatId),
            mOldestKnownMsgId.toString().c_str(), mNewestKnownMsgId.toString().c_str());
        sendCommand(Command(OP_RANGE) + mChatId + mOldestKnownMsgId + mNewestKnownMsgId);
        return;
    }
    if (empty())
    {
        CHATD_LOG_DEBUG("%s: No local history, no range to send", ID_CSTR(mChatId));
        initialFetchHistory(Id::null());
        return;
    }

    auto highest = highnum();
    Idx i = lownum();
    for (; i<=highest; i++)
    {
        auto& msg = at(i);
        if (msg.isSending())
        {
            i--;
            break;
        }
    }
    if (i < lownum()) //we have only unsent messages
        return;
    CHATD_LOG_DEBUG("%s: Sending RANGE calculated from memory buffer: %s - %s",
        ID_CSTR(mChatId), at(lownum()).id().toString().c_str(), at(i).id().toString().c_str());
    sendCommand(Command(OP_RANGE) + mChatId + at(lownum()).id() + at(i).id());
}

void Client::msgConfirm(const Id& msgxid, const Id& msgid)
{
    // CHECK: is it more efficient to keep a separate mapping of msgxid to messages?
    for (auto& messages: mMessagesForChatId)
    {
        if (messages.second->confirm(msgxid, msgid) != CHATD_IDX_INVALID)
            return;
    }
    throw std::runtime_error("msgConfirm: Unknown msgxid "+msgxid);
}

// msgid can be 0 in case of rejections
Idx Messages::confirm(const Id& msgxid, const Id& msgid)
{
    auto it = mSending.find(msgxid);
    if (it == mSending.end())
        return CHATD_IDX_INVALID;

    Message* msg = it->second.msg;
    Message* edit = it->second.edit;
    mSending.erase(it);

    if (!msgid)
    {
        CALL_DB(deleteMsgFromSending, msgxid);
        CALL_LISTENER(onMessageRejected, msgxid);
        delete msg;
        if (edit)
            delete edit;
        return CHATD_IDX_INVALID;
    }
    //update transaction id to the actual msgid
    assert(msg->mIdIsXid);
    msg->setId(msgid, false);
    push_forward(msg);
    auto idx = mIdToIndexMap[msgid] = highnum();
    printf("delete from sending: %lld\n", msgxid.val);
    CALL_DB(deleteMsgFromSending, msgxid);
    CALL_DB(addMsgToHistory, *msg, idx);
    if (edit)
    {
        edit->setEdits(msg->id(), false); //we now have a proper msgid of the original, link the edit msg to it
        CALL_DB(updateSendingEditId, msgxid, msgid);
        doMsgSubmit(edit);
    }
    CALL_LISTENER(onMessageConfirmed, msgxid, msgid, idx);
    return idx;
}

Message::Status Messages::getMsgStatus(Idx idx, const Id& userid)
{
    assert(idx != CHATD_IDX_INVALID);
    if (userid == mClient.mUserId)
    {
        auto& msg = at(idx);
        if (msg.isSending())
            return Message::kSending;
        else if (idx <= mLastReceivedIdx)
            return Message::kDelivered;
        else
        {
            return Message::kServerReceived;
        }
    } //message is from a peer
    else
    {
        return (idx <= mLastSeenIdx) ? Message::kSeen : Message::kNotSeen;
    }
}

Idx Messages::msgIncoming(bool isNew, Message* message, bool isLocal)
{
    assert((isLocal && !isNew) || !isLocal);
    auto msgid = message->id();
    assert(msgid);
    Idx idx;

    if (isNew)
    {
        push_forward(message);
        if (mOldestKnownMsgId)
            mNewestKnownMsgId = msgid; //expand the range with newer network-received messages, we don't fetch new messages from db
        idx = highnum();
    }
    else
    {
        push_back(message);
        assert(mHistFetchState & kHistFetchingFlag);
        mLastHistFetchCount++;
        if (msgid == mOldestKnownMsgId) //we have a user-set range, and we have just added the oldest message that we know we have (maybe in db), the user range is no longer up-to-date, so disable it
        {
            mOldestKnownMsgId = 0;
            mNewestKnownMsgId = 0;
        }
        idx = lownum();
    }
    mIdToIndexMap[msgid] = idx;
    if (!isLocal)
    {
        CALL_DB(addMsgToHistory, *message, idx);
    }
    if ((message->userid != mClient.mUserId) && !isLocal)
    {
        sendCommand(Command(OP_RECEIVED) + mChatId + msgid);
    }
    if (isNew)
        CALL_LISTENER(onRecvNewMessage, idx, *message, (getMsgStatus(idx, message->userid)));
    else
        CALL_LISTENER(onRecvHistoryMessage, idx, *message, (getMsgStatus(idx, message->userid)), isLocal);

    //normally the indices will not be set if mLastXXXId == msgid, as there will be only
    //one chance to set the idx (we receive the msg only once).
    if (msgid == mLastSeenId) //we didn't have the message when we received the last seen id
    {
        CHATD_LOG_DEBUG("Received the message with the last-seen msgid, setting the index pointer to it");
        onLastSeen(msgid);
    }
    if (mLastReceivedId == msgid)
    {
        //we didn't have the message when we received the last received msgid pointer,
        //and now we just received the message - set the index pointer
        CHATD_LOG_DEBUG("Received the message with the last-received msgid, setting the index pointer to it");
        onLastReceived(msgid);
    }
    return idx;
}
// procedure is as follows:
// set state as joining
// send join
// receive joins
// if (we don't have anything in DB or memory)
// {
//      call initialFetchHistory(0) - this results in sending HIST (-initialHistFetchCount)
// }
// if (server has history)
// {
//      receive server RANGE
//      call initialFetchHistory(rangeLast) - ignored if it was already called in prev step
// }
// [receive history from server, if any]
// receive HISTDONE
// login is complete, set state to online

void Messages::initialFetchHistory(Id serverNewest)
{
    if (mInitialFetchHistoryCalled)
        return;
    mInitialFetchHistoryCalled = true;
    assert(mOnlineState == kChatStateJoining);
    if (empty()) //if we have messages in db, we must always have loaded some
    {
        assert(!mOldestKnownMsgId);
        //we don't have messages in db, and we don't have messages in memory
        //we haven't sent a RANGE, so we can get history only backwards
        requestHistoryFromServer(-initialHistoryFetchCount);
    }
    else
    {
        if (at(highnum()).id() != serverNewest)
        {
            CHATD_LOG_DEBUG("%s: There are new messages on the server, requesting them", ID_CSTR(mChatId));
//the server has more recent msgs than the most recent in our db, retrieve all newer ones, after our RANGE
            requestHistoryFromServer(0x0fffffff);
        }
        else
        {
            onJoinComplete();
        }
    }
}

void Messages::onUserJoin(const Id& userid, char priv)
{
    if (priv != PRIV_NOTPRESENT)
        CALL_LISTENER(onUserJoined, userid, priv);
    else
        CALL_LISTENER(onUserLeft, userid);
}

void Messages::onJoinComplete()
{
    setOnlineState(kChatStateOnline);
    loadAndProcessUnsent();
}
void Messages::setOnlineState(ChatState state)
{
    if (state == mOnlineState)
        return;
    mOnlineState = state;
    printf("setOnlineState: %s\n", chatStateToStr(mOnlineState));
    CALL_LISTENER(onOnlineStateChange, state);
}

const char* Command::opcodeNames[] =
{
 "KEEPALIVE","JOIN", "OLDMSG", "NEWMSG", "MSGUPD(should not be used)","SEEN",
 "RECEIVED","RETENTION","HIST", "RANGE","MSGID","REJECT",
 "BROADCAST", "HISTDONE"
};
const char* Message::statusNames[] =
{
  "Sending", "ServerReceived", "ServerRejected", "Delivered", "NotSeen", "Seen"
};

}
