/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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

#ifndef nsRDFToolbarDataModel_h__
#define nsRDFToolbarDataModel_h__

#include "nsRDFDataModel.h"
#include "nsIToolbarDataModel.h"
#include "rdf.h"

////////////////////////////////////////////////////////////////////////

/**
 * An implementation for the Toolbar widget model.
 */
class nsRDFToolbarDataModel : public nsRDFDataModel, nsIToolbarDataModel {
public:
    nsRDFToolbarDataModel(nsIRDFDataBase& db, RDF_Resource& root);
    virtual ~nsRDFToolbarDataModel(void);

    ////////////////////////////////////////////////////////////////////////
    // nsISupports interface

    // XXX Note that we'll just use the parent class's implementation
    // of AddRef() and Release()

    // NS_IMETHOD_(nsrefcnt) AddRef(void);
    // NS_IMETHOD_(nsrefcnt) Release(void);
    NS_IMETHOD QueryInterface(const nsIID& iid, void** result);


    ////////////////////////////////////////////////////////////////////////
    // nsIToolbarDataModel interface



private:
    RDF_Resource& mRoot;
};



#endif // nsRDFToolbarDataModel_h__
