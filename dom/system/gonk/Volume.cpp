/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCOMPtr.h"
#include "nsIVolume.h"
#include "nsXULAppAPI.h"

#include "Volume.h"
#include "VolumeCommand.h"
#include "VolumeManager.h"
#include "VolumeManagerLog.h"

namespace mozilla {
namespace system {

class nsVolume : public nsIVolume
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIVOLUME

  nsVolume(nsAString &aName, nsAString &aMount, const long aState);

private:
  ~nsVolume();

protected:
  nsString mName;
  nsString mMount;
  long mState;
};

NS_IMPL_THREADSAFE_ISUPPORTS1(nsVolume, nsIVolume)

nsVolume::nsVolume(nsAString &aName, nsAString &aMount, const long aState)
  :mName(aName), mMount(aMount), mState(aState)
{
}

nsVolume::~nsVolume()
{
}

NS_IMETHODIMP nsVolume::GetName(nsAString & aName)
{
  aName = mName;
  return NS_OK;
}

NS_IMETHODIMP nsVolume::GetMountPoint(nsAString & aMountPoint)
{
  aMountPoint = mMount;
  return NS_OK;
}

NS_IMETHODIMP nsVolume::GetState(PRInt32 *aState)
{
  *aState = mState;
  return NS_OK;
}


Volume::Volume(const nsCSubstring &aName)
  : mState(STATE_INIT),
    mName(aName)
{
  DBG("Volume %s: created", NameStr());
}

void
Volume::SetState(Volume::STATE aNewState)
{
  if (aNewState != mState) {
    LOG("Volume %s: changing state from %s to %s (%d observers)",
        NameStr(), StateStr(mState),
        StateStr(aNewState), mEventObserverList.Length());

    mState = aNewState;
    mEventObserverList.Broadcast(ChangedEvent(this));
  }
}

void
Volume::SetMountPoint(const nsCSubstring &aMountPoint)
{
  if (!mMountPoint.Equals(aMountPoint)) {
    mMountPoint = aMountPoint;
    DBG("Volume %s: Setting mountpoint to '%s'", NameStr(), mMountPoint.get());
  }
}

void
Volume::StartMount(VolumeResponseCallback *aCallback)
{
  StartCommand(new VolumeActionCommand(this, "mount", "", aCallback));
}

void
Volume::StartUnmount(VolumeResponseCallback *aCallback)
{
  StartCommand(new VolumeActionCommand(this, "unmount", "force", aCallback));
}

void
Volume::StartShare(VolumeResponseCallback *aCallback)
{
  StartCommand(new VolumeActionCommand(this, "share", "ums", aCallback));
}

void
Volume::StartUnshare(VolumeResponseCallback *aCallback)
{
  StartCommand(new VolumeActionCommand(this, "unshare", "ums", aCallback));
}

void
Volume::StartCommand(VolumeCommand *aCommand)
{
#if 0
+  NS_ConvertUTF8toUTF16 name(mName);
+  NS_ConvertUTF8toUTF16 mount(mMountPoint);
+  nsCOMPtr<nsIVolume> v = new nsVolume(name, mount, mState);
+  printf("nsVolume = %p\n", v.get());
+
#endif
  VolumeManager::PostCommand(aCommand);
}

void
Volume::RegisterObserver(Volume::EventObserver *aObserver)
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  mEventObserverList.AddObserver(aObserver);
  // Send an initial event to the observer
  aObserver->Notify(ChangedEvent(this));
}

void
Volume::UnregisterObserver(Volume::EventObserver *aObserver)
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  mEventObserverList.RemoveObserver(aObserver);
}

Volume *
Volume::Clone() const
{
  // Create a copy of a volume object, but do it in a way that
  // none of the data is shared with the original (since this
  // clone will be sent cross-thread)
  ScopedFreePtr<Volume>  cloneVolume(new Volume(Name()));
  cloneVolume->mState = mState;
  cloneVolume->mMountPoint = MountPoint();
  return cloneVolume.forget();
}

} // namespace system
} // namespace mozilla
