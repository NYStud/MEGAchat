
/* This code is based on strophe.jingle.js by ESTOS */
#include <string>
#include <map>
#include <memory>
#include "webrtcAdapter.h"
//#include "strophe.jingle.session.h"
#include <mstrophepp.h>
namespace disco
{
    class DiscoPlugin;
}


namespace karere
{
namespace rtcModule
{
class JingleSession;
//TODO: Implement
class FileTransferHandler;
struct AnswerOptions
{
    artc::tspMediaStream localStream;
    AvFlags muted;
};
class ICryptoFunctions
{
public:
    virtual std::string generateMac(const std::string& data, const std::string& key) = 0;
    virtual std::string decryptMessage(const std::string& msg) = 0;
    virtual std::string encryptMessageForJid(const std::string& msg, const std::string& jid) = 0;
    virtual promise::Promise<int> preloadCryptoForJid(const std::string& jid) = 0;
    virtual std::string scrambleJid(const std::string& jid) = 0;
    virtual std::string generateFprMacKey() = 0;
    virtual ~ICryptoFunctions() {}
};

class Jingle: strophe::Plugin
{
protected:
/** Contains all info about a not-yet-established session, when onCallTerminated is fired and there is no session yet */
    struct FakeSessionInfo
    {
        const char* sid = NULL;
        const char* peer = NULL;
        bool isInitiator=false;
    };
/** Contains all info about an incoming call that has been accepted at the message level and needs to be autoaccepted at the jingle level */
    struct AutoAcceptCallInfo: public StringMap
    {
        Ts tsReceived;
        Ts tsTillJingle;
        std::shared_ptr<AnswerOptions> options;
        std::shared_ptr<FileTransferHandler> ftHandler;
    };
    typedef std::map<std::string, std::shared_ptr<AutoAcceptCallInfo> > AutoAcceptMap;
    std::map<std::string, JingleSession*> mSessions;
/** Timeout after which if an iq response is not received, an error is generated */
    int mJingleTimeout = 50000;
/** The period, during which an accepted call request will be valid
* and the actual call answered. The period starts at the time the
* request is received from the network */
    int mJingleAutoAcceptTimeout = 15000;
/** The period within which an outgoing call can be answered by peer */
    int callAnswerTimeout = 50000;
    AutoAcceptMap mAutoAcceptCalls;
    std::unique_ptr<ICryptoFunctions> mCrypto;
    std::string mOwnFprMacKey;
    std::string mOwnAnonId;
public:
    enum {DISABLE_MIC = 1, DISABLE_CAM = 2, HAS_MIC = 4, HAS_CAM = 8};
    int mediaFlags = 0;

    std::shared_ptr<webrtc::PeerConnectionInterface::IceServers> mIceServers;
    webrtc::FakeConstraints mMediaConstraints;
    std::shared_ptr<artc::InputDevices> mInputDevices;

//event handler interface
    virtual void onConnectionEvent(int state, const std::string& msg){}
    virtual void onRemoteStreamAdded(JingleSession& sess, artc::tspMediaStream stream){}
    virtual void onRemoteStreamRemoved(JingleSession& sess, artc::tspMediaStream stream){}
    virtual void onJingleError(JingleSession& sess, const std::string& err, strophe::Stanza stanza, strophe::Stanza orig){}
    virtual void onJingleTimeout(JingleSession& sess, const std::string& err, strophe::Stanza orig){}
//    virtual void onIceConnStateChange(JingleSession& sess, event){}
    virtual void onIceComplete(JingleSession& sess){}
//rtcHandler callback interface, called by the connection.jingle object
    virtual void onIncomingCallRequest(const char* from, std::function<bool()> reqStillValid,
      std::function<bool(bool, std::shared_ptr<AnswerOptions>, const std::string& reason,
      const std::string& text)>)
    {}
    virtual void onCallCanceled(const char* peer, const char* event,
     const char* by, bool accepted){}
    virtual void onCallRequestTimeout(const char* peer) {}
    virtual void onCallAnswered(const char* peer) {}
    virtual void onCallTerminated(JingleSession* sess, const char* reason,
      const char* text, const FakeSessionInfo* info=NULL){}
    virtual bool onCallIncoming(JingleSession& sess, std::string& reason,
                                std::string& text){return true;}
    virtual void onRinging(JingleSession& sess){}
    virtual void onMuted(JingleSession& sess, const AvFlags& affected){}
    virtual void onUnmuted(JingleSession& sess, const AvFlags& affected){}
    virtual void onInternalError(const std::string& msg, const char* where);
//==
    std::string generateHmac(const std::string& data, const std::string& key);
    Jingle(strophe::Connection& conn, ICryptoFunctions* crypto, const std::string& iceServers="");
    void addAudioCaps(disco::DiscoPlugin& disco);
    void addVideoCaps(disco::DiscoPlugin& disco);
    void registerDiscoCaps();
    void setIceServers(const std::string& servers){}
    void onConnState(const xmpp_conn_event_t status,
        const int error, xmpp_stream_error_t * const stream_error);
/*    int _static_onJingle(xmpp_conn_t* const conn,
        xmpp_stanza_t* stanza, void* userdata);
    static int _static_onIncomingCallMsg(xmpp_conn_t* const conn,
        xmpp_stanza_t* stanza, void* userdata);
*/
    bool onJingle(strophe::Stanza iq);
    /* Incoming call request with a message stanza of type 'megaCall' */
    bool onIncomingCallMsg(strophe::Stanza callmsg);
    bool cancelAutoAcceptEntry(const char* sid, const char* reason,
        const char* text, char type=0);
    bool cancelAutoAcceptEntry(AutoAcceptMap::iterator it, const char* reason,
    const char* text, char type=0);
    void cancelAllAutoAcceptEntries(const char* reason, const char* text);
    void purgeOldAcceptCalls();
    void processAndDeleteInputQueue(JingleSession& sess);
    promise::Promise<std::shared_ptr<JingleSession> >
      initiate(const char* sid, const char* peerjid, const char* myjid,
        artc::tspMediaStream sessStream, const AvFlags& mutedState,
        std::shared_ptr<StringMap> sessProps, FileTransferHandler* ftHandler=NULL);
    JingleSession* createSession(const char* me, const char* peerjid,
        const char* sid, artc::tspMediaStream, const AvFlags& mutedState,
        const StringMap& sessProps, FileTransferHandler* ftHandler=NULL);
    void terminateAll(const char* reason, const char* text, bool nosend=false);
    bool terminateBySid(const char* sid, const char* reason, const char* text,
        bool nosend=false);
    bool terminate(JingleSession* sess, const char* reason, const char* text,
        bool nosend=false);
    promise::Promise<strophe::Stanza> sendTerminateNoSession(const char* sid,
        const char* to, const char* reason, const char* text);
    bool sessionIsValid(const karere::rtcModule::JingleSession &sess);
    std::string getFingerprintsFromJingle(strophe::Stanza j);
};
}
}
