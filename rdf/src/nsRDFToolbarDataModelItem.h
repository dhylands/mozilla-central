/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#ifndef nsRDFTreeDataModelItem_h__
#define nsRDFTreeDataModelItem_h__

#include "rdf.h"
#include "nsRDFDataModelItem.h"
#include "nsIToolbarDMItem.h"

class nsRDFToolbarDataModel;

////////////////////////////////////////////////////////////////////////

class nsRDFToolbarDataModelItem : public nsRDFDataModelItem, nsIToolbarDMItem {
public:
    nsRDFToolbarDataModelItem(nsRDFToolbarDataModel& Toolbar, RDF_Resource& resource);
    virtual ~nsRDFToolbarDataModelItem(void);

    ////////////////////////////////////////////////////////////////////////
    // nsISupports interface

    // XXX Note that we'll just use the parent class's implementation
    // of AddRef() and Release()
    // NS_IMETHOD_(nsrefcnt) AddRef(void);
    // NS_IMETHOD_(nsrefcnt) Release(void);
    NS_IMETHOD QueryInterface(const nsIID& iid, void** result);

    ////////////////////////////////////////////////////////////////////////
    // nsIToolbarItem interface


private:
    nsRDFToolbarDataModel&  mToolbar;
};

////////////////////////////////////////////////////////////////////////


#endif // nsRDFToolbarDataModelItem_h__
