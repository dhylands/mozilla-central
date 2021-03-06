/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Preferences_h
#define mozilla_Preferences_h

#ifndef MOZILLA_INTERNAL_API
#error "This header is only usable from within libxul (MOZILLA_INTERNAL_API)."
#endif

#include "nsIPrefService.h"
#include "nsIPrefBranch.h"
#include "nsIPrefBranchInternal.h"
#include "nsIObserver.h"
#include "nsCOMPtr.h"
#include "nsWeakReference.h"

class nsIFile;
class nsCString;
class nsString;
class nsAdoptingString;
class nsAdoptingCString;

#ifndef have_PrefChangedFunc_typedef
typedef int (*PR_CALLBACK PrefChangedFunc)(const char *, void *);
#define have_PrefChangedFunc_typedef
#endif

namespace mozilla {

class Preferences : public nsIPrefService,
                    public nsIObserver,
                    public nsIPrefBranchInternal,
                    public nsSupportsWeakReference
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIPREFSERVICE
  NS_FORWARD_NSIPREFBRANCH(sRootBranch->)
  NS_DECL_NSIOBSERVER

  Preferences();
  virtual ~Preferences();

  nsresult Init();

  /**
   * Reset loaded user prefs then read them
   */
  static nsresult ResetAndReadUserPrefs();

  /**
   * Returns the singleton instance which is addreffed.
   */
  static Preferences* GetInstanceForService();

  /**
   * Finallizes global members.
   */
  static void Shutdown();

  /**
   * Returns shared pref service instance
   * NOTE: not addreffed.
   */
  static nsIPrefService* GetService()
  {
    NS_ENSURE_TRUE(InitStaticMembers(), nsnull);
    return sPreferences;
  }

  /**
   * Returns shared pref branch instance.
   * NOTE: not addreffed.
   */
  static nsIPrefBranch* GetRootBranch()
  {
    NS_ENSURE_TRUE(InitStaticMembers(), nsnull);
    return sRootBranch;
  }

  /**
   * Returns shared default pref branch instance.
   * NOTE: not addreffed.
   */
  static nsIPrefBranch* GetDefaultRootBranch()
  {
    NS_ENSURE_TRUE(InitStaticMembers(), nsnull);
    return sDefaultRootBranch;
  }

  /**
   * Gets int or bool type pref value with default value if failed to get
   * the pref.
   */
  static bool GetBool(const char* aPref, bool aDefault = false)
  {
    bool result = aDefault;
    GetBool(aPref, &result);
    return result;
  }

  static PRInt32 GetInt(const char* aPref, PRInt32 aDefault = 0)
  {
    PRInt32 result = aDefault;
    GetInt(aPref, &result);
    return result;
  }

  static PRUint32 GetUint(const char* aPref, PRUint32 aDefault = 0)
  {
    PRUint32 result = aDefault;
    GetUint(aPref, &result);
    return result;
  }

  /**
   * Gets char type pref value directly.  If failed, the get() of result
   * returns NULL.  Even if succeeded but the result was empty string, the
   * get() does NOT return NULL.  So, you can check whether the method
   * succeeded or not by:
   *
   * nsAdoptingString value = Prefereces::GetString("foo.bar");
   * if (!value) {
   *   // failed
   * }
   *
   * Be aware.  If you wrote as:
   *
   * nsAutoString value = Preferences::GetString("foo.bar");
   * if (!value.get()) {
   *   // the condition is always FALSE!!
   * }
   *
   * The value.get() doesn't return NULL. You must use nsAdoptingString when
   * you need to check whether it was failure or not.
   */
  static nsAdoptingCString GetCString(const char* aPref);
  static nsAdoptingString GetString(const char* aPref);
  static nsAdoptingCString GetLocalizedCString(const char* aPref);
  static nsAdoptingString GetLocalizedString(const char* aPref);

  /**
   * Gets int or bool type pref value with raw return value of nsIPrefBranch.
   *
   * @param aPref       A pref name.
   * @param aResult     Must not be NULL.  The value is never modified when
   *                    these methods fail.
   */
  static nsresult GetBool(const char* aPref, bool* aResult);
  static nsresult GetInt(const char* aPref, PRInt32* aResult);
  static nsresult GetUint(const char* aPref, PRUint32* aResult)
  {
    PRInt32 result;
    nsresult rv = GetInt(aPref, &result);
    if (NS_SUCCEEDED(rv)) {
      *aResult = static_cast<PRUint32>(result);
    }
    return rv;
  }

  /**
   * Gets string type pref value with raw return value of nsIPrefBranch.
   *
   * @param aPref       A pref name.
   * @param aResult     Must not be NULL.  The value is never modified when
   *                    these methods fail.
   */
  static nsresult GetCString(const char* aPref, nsACString* aResult);
  static nsresult GetString(const char* aPref, nsAString* aResult);
  static nsresult GetLocalizedCString(const char* aPref, nsACString* aResult);
  static nsresult GetLocalizedString(const char* aPref, nsAString* aResult);

  static nsresult GetComplex(const char* aPref, const nsIID &aType,
                             void** aResult);

  /**
   * Sets various type pref values.
   */
  static nsresult SetBool(const char* aPref, bool aValue);
  static nsresult SetInt(const char* aPref, PRInt32 aValue);
  static nsresult SetUint(const char* aPref, PRUint32 aValue)
  {
    return SetInt(aPref, static_cast<PRInt32>(aValue));
  }
  static nsresult SetCString(const char* aPref, const char* aValue);
  static nsresult SetCString(const char* aPref, const nsACString &aValue);
  static nsresult SetString(const char* aPref, const PRUnichar* aValue);
  static nsresult SetString(const char* aPref, const nsAString &aValue);

  static nsresult SetComplex(const char* aPref, const nsIID &aType,
                             nsISupports* aValue);

  /**
   * Clears user set pref.
   */
  static nsresult ClearUser(const char* aPref);

  /**
   * Whether the pref has a user value or not.
   */
  static bool HasUserValue(const char* aPref);

  /**
   * Gets the type of the pref.
   */
  static PRInt32 GetType(const char* aPref);

  /**
   * Adds/Removes the observer for the root pref branch.
   * The observer is referenced strongly if AddStrongObserver is used.  On the
   * other hand, it is referenced weakly, if AddWeakObserver is used.
   * See nsIPrefBran2.idl for the detail.
   */
  static nsresult AddStrongObserver(nsIObserver* aObserver, const char* aPref);
  static nsresult AddWeakObserver(nsIObserver* aObserver, const char* aPref);
  static nsresult RemoveObserver(nsIObserver* aObserver, const char* aPref);

  /**
   * Adds/Removes two or more observers for the root pref branch.
   * Pass to aPrefs an array of const char* whose last item is NULL.
   */
  static nsresult AddStrongObservers(nsIObserver* aObserver,
                                     const char** aPrefs);
  static nsresult AddWeakObservers(nsIObserver* aObserver,
                                   const char** aPrefs);
  static nsresult RemoveObservers(nsIObserver* aObserver,
                                  const char** aPrefs);

  /**
   * Registers/Unregisters the callback function for the aPref.
   */
  static nsresult RegisterCallback(PrefChangedFunc aCallback,
                                   const char* aPref,
                                   void* aClosure = nsnull);
  static nsresult UnregisterCallback(PrefChangedFunc aCallback,
                                     const char* aPref,
                                     void* aClosure = nsnull);

  /**
   * Adds the aVariable to cache table.  aVariable must be a pointer for a
   * static variable.  The value will be modified when the pref value is
   * changed but note that even if you modified it, the value isn't assigned to
   * the pref.
   */
  static nsresult AddBoolVarCache(bool* aVariable,
                                  const char* aPref,
                                  bool aDefault = false);
  static nsresult AddIntVarCache(PRInt32* aVariable,
                                 const char* aPref,
                                 PRInt32 aDefault = 0);
  static nsresult AddUintVarCache(PRUint32* aVariable,
                                  const char* aPref,
                                  PRUint32 aDefault = 0);

  /**
   * Gets the default bool, int or uint value of the pref.
   * The result is raw result of nsIPrefBranch::Get*Pref().
   * If the pref could have any value, you needed to use these methods.
   * If not so, you could use below methods.
   */
  static nsresult GetDefaultBool(const char* aPref, bool* aResult);
  static nsresult GetDefaultInt(const char* aPref, PRInt32* aResult);
  static nsresult GetDefaultUint(const char* aPref, PRUint32* aResult)
  {
    return GetDefaultInt(aPref, reinterpret_cast<PRInt32*>(aResult));
  }

  /**
   * Gets the default bool, int or uint value of the pref directly.
   * You can set an invalid value of the pref to aFailedResult.  If these
   * methods failed to get the default value, they would return the
   * aFailedResult value.
   */
  static bool GetDefaultBool(const char* aPref, bool aFailedResult)
  {
    bool result;
    return NS_SUCCEEDED(GetDefaultBool(aPref, &result)) ? result :
                                                          aFailedResult;
  }
  static PRInt32 GetDefaultInt(const char* aPref, PRInt32 aFailedResult)
  {
    PRInt32 result;
    return NS_SUCCEEDED(GetDefaultInt(aPref, &result)) ? result : aFailedResult;
  }
  static PRUint32 GetDefaultUint(const char* aPref, PRUint32 aFailedResult)
  {
   return static_cast<PRUint32>(
     GetDefaultInt(aPref, static_cast<PRInt32>(aFailedResult)));
  }

  /**
   * Gets the default value of the char type pref.
   * If the get() of the result returned NULL, that meant the value didn't
   * have default value.
   *
   * See the comment at definition at GetString() and GetCString() for more
   * details of the result.
   */
  static nsAdoptingString GetDefaultString(const char* aPref);
  static nsAdoptingCString GetDefaultCString(const char* aPref);
  static nsAdoptingString GetDefaultLocalizedString(const char* aPref);
  static nsAdoptingCString GetDefaultLocalizedCString(const char* aPref);

  static nsresult GetDefaultCString(const char* aPref, nsACString* aResult);
  static nsresult GetDefaultString(const char* aPref, nsAString* aResult);
  static nsresult GetDefaultLocalizedCString(const char* aPref,
                                             nsACString* aResult);
  static nsresult GetDefaultLocalizedString(const char* aPref,
                                            nsAString* aResult);

  static nsresult GetDefaultComplex(const char* aPref, const nsIID &aType,
                                    void** aResult);

  /**
   * Gets the type of the pref.
   */
  static PRInt32 GetDefaultType(const char* aPref);

  // Used to synchronise preferences between chrome and content processes.
  static void MirrorPreferences(nsTArray<PrefTuple,
                                nsTArrayInfallibleAllocator> *aArray);
  static bool MirrorPreference(const char *aPref, PrefTuple *aTuple);
  static void ClearContentPref(const char *aPref);
  static void SetPreference(const PrefTuple *aTuple);

protected:
  nsresult NotifyServiceObservers(const char *aSubject);
  /**
   * Reads the default pref file or, if that failed, try to save a new one.
   *
   * @return NS_OK if either action succeeded,
   *         or the error code related to the read attempt.
   */
  nsresult UseDefaultPrefFile();
  nsresult UseUserPrefFile();
  nsresult ReadAndOwnUserPrefFile(nsIFile *aFile);
  nsresult ReadAndOwnSharedUserPrefFile(nsIFile *aFile);
  nsresult SavePrefFileInternal(nsIFile* aFile);
  nsresult WritePrefFile(nsIFile* aFile);
  nsresult MakeBackupPrefFile(nsIFile *aFile);

private:
  nsCOMPtr<nsIFile>        mCurrentFile;

  static Preferences*      sPreferences;
  static nsIPrefBranch*    sRootBranch;
  static nsIPrefBranch*    sDefaultRootBranch;
  static bool              sShutdown;

  /**
   * Init static members.  TRUE if it succeeded.  Otherwise, FALSE.
   */
  static bool InitStaticMembers();
};

} // namespace mozilla

#endif // mozilla_Preferences_h
