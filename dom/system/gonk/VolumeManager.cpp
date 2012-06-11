/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <android/log.h>
#include <cutils/sockets.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "base/message_loop.h"
#include "mozilla/Scoped.h"
#include "nsWhitespaceTokenizer.h"
#include "nsXULAppAPI.h"

#include "Volume.h"
#include "VolumeCommand.h"
#include "VolumeManager.h"
#include "VolumeManagerLog.h"

//using namespace mozilla::dom::gonk;

namespace mozilla {
namespace system {

static RefPtr<VolumeManager> sVolumeManager;

#define TEST_VOLUME_OBSERVER  0

/***************************************************************************/

VolumeManager::VolumeManager()
  : mState(VolumeManager::STARTING),
    mSocket(-1),
    mCommandPending(false),
    mRcvIdx(0)
{
  DBG("VolumeManager constructor called");
}

VolumeManager::~VolumeManager()
{
}

//static
VolumeManager::STATE
VolumeManager::State()
{
  if (!sVolumeManager) {
    return VolumeManager::UNINITIALIZED;
  }
  return sVolumeManager->mState;
}

//static
const char *
VolumeManager::StateStr()
{
  switch (State()) {
    case UNINITIALIZED: return "Uninitialized";
    case STARTING:      return "Starting";
    case VOLUMES_READY: return "Volumes Ready";
  }
  return "???";
}


//static
void
VolumeManager::SetState(STATE aNewState)
{
  if (!sVolumeManager) {
    return;
  }
  if (sVolumeManager->mState != aNewState) {
    sVolumeManager->mState = aNewState;
    sVolumeManager->mStateObserverList.Broadcast(StateChangedEvent());
  }
}

//static
void
VolumeManager::RegisterStateObserver(StateObserver *aObserver)
{
  sVolumeManager->mStateObserverList.AddObserver(aObserver);
}

//static
void VolumeManager::UnregisterStateObserver(StateObserver *aObserver)
{
  sVolumeManager->mStateObserverList.RemoveObserver(aObserver);
}

//static
TemporaryRef<Volume>
VolumeManager::FindVolumeByName(const nsCSubstring &aName)
{
  if (!sVolumeManager) {
    return NULL;
  }
  for (VolumeArray::iterator volIter = sVolumeManager->mVolumeArray.begin();
       volIter != sVolumeManager->mVolumeArray.end();
       volIter++) {
    if ((*volIter)->Name().Equals(aName)) {
      return *volIter;
    }
  }
  return NULL;
}

//static
TemporaryRef<Volume>
VolumeManager::FindAddVolumeByName(const nsCSubstring &aName)
{
  RefPtr<Volume> vol = FindVolumeByName(aName);
  if (vol) {
    return vol;
  }
  // No volume found, create and add a new one.
  vol = new Volume(aName);
  sVolumeManager->mVolumeArray.push_back(vol);
  return vol;
}

class VolumeListCallback : public VolumeResponseCallback
{
  virtual void ResponseReceived(const VolumeCommand *aCommand)
  {
    switch (ResponseCode()) {
      case ResponseCode::VolumeListResult: {
        // Each line will look something like:
        //
        //  sdcard /mnt/sdcard 1
        //
        // So for each volume that we get back, we update any volumes that
        // we have of the same name, or add new ones if they don't exist.
        nsCWhitespaceTokenizer tokenizer(ResponseStr());
        nsDependentCSubstring volName(tokenizer.nextToken());
        nsDependentCSubstring mntPoint(tokenizer.nextToken());
        nsCString state(tokenizer.nextToken());
        RefPtr<Volume> vol = VolumeManager::FindAddVolumeByName(volName);
        vol->SetMountPoint(mntPoint);
        PRInt32 errCode;
        vol->SetState((Volume::STATE)state.ToInteger(&errCode));
        break;
      }

      case ResponseCode::CommandOkay: {
        // We've received the list of volumes. Tell anybody who
        // is listening that we're open for business.
        VolumeManager::SetState(VolumeManager::VOLUMES_READY);
        break;
      }
    }
  }
};

bool
VolumeManager::OpenSocket()
{
  SetState(STARTING);
  if ((mSocket.rwget() = socket_local_client("vold",
                                             ANDROID_SOCKET_NAMESPACE_RESERVED,
                                             SOCK_STREAM)) < 0) {
      ERR("Error connecting to vold: (%s) - will retry", strerror(errno));
      return false;
  }
  // add FD_CLOEXEC flag
  int flags = fcntl(mSocket.get(), F_GETFD);
  if (flags == -1) {
      return false;
  }
  flags |= FD_CLOEXEC;
  if (fcntl(mSocket.get(), F_SETFD, flags) == -1) {
    return false;
  }
  // set non-blocking
  if (fcntl(mSocket.get(), F_SETFL, O_NONBLOCK) == -1) {
    return false;
  }
  if (!MessageLoopForIO::current()->
      WatchFileDescriptor(mSocket.get(),
                          true,
                          MessageLoopForIO::WATCH_READ,
                          &mReadWatcher,
                          this)) {
      return false;
  }

  LOG("Connected to vold");
  PostCommand(new VolumeListCommand(new VolumeListCallback));
  return true;
}

//static
void
VolumeManager::PostCommand(VolumeCommand *aCommand)
{
  if (!sVolumeManager) {
    ERR("VolumeManager not initialized. Dropping command '%s'", aCommand->Data());
    return;
  }
  aCommand->SetPending(true);

  DBG("Sending command '%s'", aCommand->Data());
  // vold can only process one command at a time, so add our command
  // to the end of the command queue.
  sVolumeManager->mCommands.push(aCommand);
  if (!sVolumeManager->mCommandPending) {
    // There aren't any commands currently being processed, so go
    // ahead and kick this one off.
    sVolumeManager->mCommandPending = true;
    sVolumeManager->WriteCommandData();
  }
}

/***************************************************************************
* The WriteCommandData initiates sending of a command to vold. Since
* we're running on the IOThread and not allowed to block, WriteCommandData
* will write as much data as it can, and if not all of the data can be
* written then it will setup a file descriptor watcher and
* OnFileCanWriteWithoutBlocking will call WriteCommandData to write out
* more of the command data.
*/
void
VolumeManager::WriteCommandData()
{
  if (mCommands.size() == 0) {
    return;
  }

  VolumeCommand *cmd = mCommands.front();
  if (cmd->BytesRemaining() == 0) {
    // All bytes have been written. We're waiting for a response.
    return;
  }
  // There are more bytes left to write. Try to write them all.
  ssize_t bytesWritten = write(mSocket.get(), cmd->Data(), cmd->BytesRemaining());
  if (bytesWritten < 0) {
    ERR("Failed to write %d bytes to vold socket", cmd->BytesRemaining());
    Restart();
    return;
  }
  DBG("Wrote %ld bytes (of %d)", bytesWritten, cmd->BytesRemaining());
  cmd->ConsumeBytes(bytesWritten);
  if (cmd->BytesRemaining() == 0) {
    return;
  }
  // We were unable to write all of the command bytes. Setup a watcher
  // so we'll get called again when we can write without blocking.
  if (!MessageLoopForIO::current()->
      WatchFileDescriptor(mSocket.get(),
                          false, // one-shot
                          MessageLoopForIO::WATCH_WRITE,
                          &mWriteWatcher,
                          this)) {
    ERR("Failed to setup write watcher for vold socket");
    Restart();
  }
}

void
VolumeManager::OnFileCanReadWithoutBlocking(int aFd)
{
  MOZ_ASSERT(aFd == mSocket.get());
  while (true) {
    ssize_t bytesRemaining = read(aFd, &mRcvBuf[mRcvIdx], sizeof(mRcvBuf) - mRcvIdx);
    if (bytesRemaining < 0) {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      ERR("Unknown read error: %d (%s) - restarting", errno, strerror(errno));
      Restart();
      return;
    }
    if (bytesRemaining == 0) {
      // This means that vold probably crashed
      ERR("Vold appears to have crashed - restarting");
      Restart();
      return;
    }
    // We got some data. Each line is terminated by a null character
    DBG("Read %ld bytes", bytesRemaining);
    while (bytesRemaining > 0) {
      bytesRemaining--;
      if (mRcvBuf[mRcvIdx] == '\0') {
        // We found a line terminator. Each line is formatted as an
        // integer response code followed by the rest of the line.
        // Fish out the response code.
        char *endPtr;
        int responseCode = strtol(mRcvBuf, &endPtr, 10);
        if (*endPtr == ' ') {
          endPtr++;
        }

        // Now fish out the rest of the line after the response code
        nsDependentCString  responseLine(endPtr, &mRcvBuf[mRcvIdx] - endPtr);
        DBG("Rcvd: %d '%s'", responseCode, responseLine.Data());

        if (responseCode >= ResponseCode::UnsolicitedInformational) {
          // These are unsolicited broadcasts. We intercept these and process
          // them ourselves
          HandleBroadcast(responseCode, responseLine);
        } else {
          // Everything else is considered to be part of the command response.
          if (mCommands.size() > 0) {
            VolumeCommand *cmd = mCommands.front();
            cmd->HandleResponse(responseCode, responseLine);
            if (responseCode >= ResponseCode::CommandOkay) {
              // That's a terminating response. We can remove the command.
              mCommands.pop();
              mCommandPending = false;
              // Start the next command, if there is one.
              WriteCommandData();
            }
          } else {
            ERR("Response with no command");
          }
        }
        if (bytesRemaining > 0) {
          // There is data in the receive buffer beyond the current line.
          // Shift it down to the beginning.
          memmove(&mRcvBuf[0], &mRcvBuf[mRcvIdx + 1], bytesRemaining);
        }
        mRcvIdx = 0;
      } else {
        mRcvIdx++;
      }
    }
  }
}

void
VolumeManager::OnFileCanWriteWithoutBlocking(int aFd)
{
  MOZ_ASSERT(aFd == mSocket.get());
  WriteCommandData();
}

void
VolumeManager::HandleBroadcast(int aResponseCode, nsCString &aResponseLine)
{
  if (aResponseCode != ResponseCode::VolumeStateChange) {
    return;
  }
  // Format of the line is something like:
  //
  //  Volume sdcard /mnt/sdcard state changed from 7 (Shared-Unmounted) to 1 (Idle-Unmounted)
  //
  // So we parse out the volume name and the state after the string " to "
  nsCWhitespaceTokenizer  tokenizer(aResponseLine);
  tokenizer.nextToken();  // The word "Volume"
  nsDependentCSubstring volName(tokenizer.nextToken());

  RefPtr<Volume> vol = FindVolumeByName(volName);
  if (!vol) {
    return;
  }

  const char *s = strstr(aResponseLine.get(), " to ");

  if (!s) {
    return;
  }
  s += 4;
  vol->SetState((Volume::STATE)atoi(s));
}

void
VolumeManager::Restart()
{
  mReadWatcher.StopWatchingFileDescriptor();
  mWriteWatcher.StopWatchingFileDescriptor();

  while (!mCommands.empty()) {
    mCommands.pop();
  }
  mCommandPending = false;
  mSocket.dispose();
  mRcvIdx = 0;
  Start();
}

//static
void
VolumeManager::Start()
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  if (!sVolumeManager) {
    return;
  }
  if (!sVolumeManager->OpenSocket()) {
    // Socket open failed, try again in a second.
    MessageLoopForIO::current()->
      PostDelayedTask(FROM_HERE,
                      NewRunnableFunction(VolumeManager::Start),
                      1000);
  }
}
#if TEST_VOLUME_OBSERVER
class TestVolumeEventObserver : public VolumeEventObserver
{
public:
  virtual void Notify(const Volume::ChangedEvent &aEvent)
  {
    const Volume *vol = aEvent.GetVolume();
    LOG("Volume Changed event '%s' state %s mountpoint %s",
        vol->NameStr(), vol->StateStr(), vol->MountPoint().get());
  }
};

static TestVolumeEventObserver sTestVolumeEventObserver;
static MessageLoop *sVolumeObserverTestThread;

static void
TestVolumeObserver()
{
  LOG("%s: About to call RegisterVolumeEventObserver", __FUNCTION__ );
  RegisterVolumeEventObserver(NS_LITERAL_CSTRING("sdcard"),
                              &sTestVolumeEventObserver);
}
#endif // TEST_VOLUME_OBSERVER

/***************************************************************************/

static void
InitVolumeManagerIOThread()
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());
  MOZ_ASSERT(!sVolumeManager);

  sVolumeManager = new VolumeManager();
  VolumeManager::Start();

#if TEST_VOLUME_OBSERVER
  sVolumeObserverTestThread->PostTask(
     FROM_HERE,
     NewRunnableFunction(TestVolumeObserver));
#endif
}

static void
ShutdownVolumeManagerIOThread()
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  sVolumeManager = NULL;
}

class VolumeEventObserverProxy : public Volume::EventObserver
{
public:
  VolumeEventObserverProxy(VolumeEventObserver *aRemoteObserver)
    : mRemoteObserver(aRemoteObserver) {}

  virtual void Notify(const Volume::ChangedEvent &aEvent);
private:
  VolumeEventObserver *mRemoteObserver;
};

// This notify function runs in the context of the thread which
// called RegisterVolumeEventObserver
static void
NotifyVolumeEventRemoteThread(VolumeEventObserver *aRemoteObserver,
                              Volume *aVolume)
{
  // aVolume is the cloned volume allocated in the IOThread.
  // We introduce the RefPtr here (now that we're running on the
  // remote thread) and it will free the memory.
  RefPtr<Volume> volume(aVolume);
  Volume::ChangedEvent remoteChangedEvent(volume);
  aRemoteObserver->Notify(remoteChangedEvent);
}

// This Notify function runs in the IOThread
void
VolumeEventObserverProxy::Notify(const Volume::ChangedEvent &aEvent)
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  if (!mRemoteObserver) {
    ERR("%s: No remote observer", __FUNCTION__);
    return;
  }

  // We're running in the IOThread. We need to do the real notification
  // on the remote observers thread.
  MessageLoop  *messageLoop = mRemoteObserver->GetMessageLoop();
  if (!messageLoop) {
    ERR("%s: No message loop", __FUNCTION__);
    return;
  }

  // We don't want to send a RefPtr to the volume cross-thread (since there
  // could be races when adjusting the reference count), so we create a
  // copy and send the copy.
  messageLoop->PostTask(
      FROM_HERE,
      NewRunnableFunction(NotifyVolumeEventRemoteThread, mRemoteObserver,
                          aEvent.GetVolume()->Clone()));
}

static void
RegisterVolumeEventObserverIOThread(nsCString * const &aVolumeName,
                                    VolumeEventObserver * const &aRemoteObserver)
{
  ScopedFreePtr<nsCString> ioVolumeName(aVolumeName);

  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  // We use FindAddVolumeByName to cover the case where the caller wants to
  // register for volume events before we've gotten the volume list back
  // from vold. Once the VolumeManager gets the volume list, it also uses
  // FindAddVolumeByName, so that way the order doesn't matter.
  RefPtr<Volume>  volume = VolumeManager::FindAddVolumeByName(*ioVolumeName);
  if (!volume) {
    return;
  }
  ScopedFreePtr<Volume::EventObserver> proxyObserver(
     new VolumeEventObserverProxy(aRemoteObserver));

  aRemoteObserver->SetProxyObserver(proxyObserver);
  volume->RegisterObserver(proxyObserver);
  proxyObserver.forget();
}

static void
UnregisterVolumeEventObserverIOThread(nsCString * const &aVolumeName,
                                      Volume::EventObserver * const &aProxyObserver)
{
  ScopedFreePtr<nsCString> ioVolumeName(aVolumeName);
  ScopedFreePtr<Volume::EventObserver> proxyObserver(aProxyObserver);

  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  if (!proxyObserver) {
    return;
  }
  RefPtr<Volume>  volume = VolumeManager::FindVolumeByName(*ioVolumeName);
  if (!volume) {
    return;
  }
  volume->UnregisterObserver(proxyObserver);
}

/**************************************************************************
*
*   Public API
*
*   Since the VolumeManager runs in IO Thread context, we need to switch
*   to IOThread context before we can do anything.
*
**************************************************************************/

void
InitVolumeManager()
{
#if TEST_VOLUME_OBSERVER
  sVolumeObserverTestThread = MessageLoop::current();
#endif

  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(InitVolumeManagerIOThread));
}

void
ShutdownVolumeManager()
{
  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(ShutdownVolumeManagerIOThread));
}

void
RegisterVolumeEventObserver(const nsACString &aVolumeName,
                            VolumeEventObserver *aRemoteObserver)
{
  // We need to allocate a copy of the volumeName so that we can
  // guarantee it stays in scope until the IOThread runs.
  nsCString *ioVolumeName = new nsCString(aVolumeName);

  aRemoteObserver->SetMessageLoop();
  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(RegisterVolumeEventObserverIOThread,
                          ioVolumeName, aRemoteObserver));
}

void
UnregisterVolumeEventObserver(const nsACString &aVolumeName,
                              VolumeEventObserver *aRemoteObserver)
{
  // We need to allocate a copy of the volumeName so that we can
  // guarantee it stays in scope until the IOThread runs
  nsCString *ioVolumeName = new nsCString(aVolumeName);

  // We also can't be sure that the remote observer will stay
  // in scope, so we grab the proxy and pass it instead.
  Volume::EventObserver *proxyObserver = aRemoteObserver->GetProxyObserver();

  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(UnregisterVolumeEventObserverIOThread,
                          ioVolumeName, proxyObserver));
}

} // system
} // mozilla

