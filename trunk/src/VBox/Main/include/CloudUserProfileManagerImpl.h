/* $Id$ */
/** @file
 * VirtualBox COM class implementation
 */

/*
 * Copyright (C) 2006-2018 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


#ifndef ____H_CLOUDUSERPROFILEMANAGERIMPL
#define ____H_CLOUDUSERPROFILEMANAGERIMPL

/* VBox includes */
#include "CloudProviderManagerWrap.h"
#include "CloudUserProfilesImpl.h"

/* VBox forward declarations */

class ATL_NO_VTABLE CloudProviderManager
    : public CloudProviderManagerWrap
{
public:

    DECLARE_EMPTY_CTOR_DTOR(CloudProviderManager)

    HRESULT FinalConstruct();
    void FinalRelease();

    HRESULT init(VirtualBox *aVirtualBox);
    void uninit();

private:
    ComPtr<VirtualBox> const mParent;       /**< Strong reference to the parent object (VirtualBox/IMachine). */
#ifdef VBOX_WITH_CLOUD_PROVIDERS_IN_EXTPACK
    std::vector<ComPtr<ICloudProviderManager>> mUserProfileManagers;
#else
    std::vector<Utf8Str> mSupportedProviders;
#endif

    HRESULT getSupportedProviders(std::vector<Utf8Str> &aProviderTypes);
    HRESULT getAllProfiles(std::vector< ComPtr<ICloudProvider> > &aProfilesList);
    HRESULT getProfilesByProvider(const com::Utf8Str &aProviderName, ComPtr<ICloudProvider> &aProfiles);
};

#endif // !____H_CLOUDUSERPROFILEMANAGERIMPL
/* vi: set tabstop=4 shiftwidth=4 expandtab: */
