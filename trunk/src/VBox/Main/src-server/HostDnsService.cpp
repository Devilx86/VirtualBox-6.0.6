/* $Id$ */
/** @file
 * Base class fo Host DNS & Co services.
 */

/*
 * Copyright (C) 2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include <VBox/com/array.h>
#include <VBox/com/ptr.h>
#include <VBox/com/string.h>

#include <iprt/cpp/utils.h>

#include "VirtualBoxImpl.h"
#include <iprt/thread.h>
#include <iprt/semaphore.h>
#include <iprt/critsect.h>

#include <algorithm>
#include <string>
#include "HostDnsService.h"


static HostDnsMonitor *g_monitor;


/* Lockee */
Lockee::Lockee()
{
    RTCritSectInit(&mLock);
}

Lockee::~Lockee()
{
    RTCritSectDelete(&mLock);
}

const RTCRITSECT* Lockee::lock() const
{
    return &mLock;
}

/* ALock */
ALock::ALock(const Lockee *aLockee)
  : lockee(aLockee)
{
    RTCritSectEnter(const_cast<PRTCRITSECT>(lockee->lock()));
}

ALock::~ALock()
{
    RTCritSectLeave(const_cast<PRTCRITSECT>(lockee->lock()));
}

inline static void detachVectorOfString(const std::vector<std::string>& v,
                                        ComSafeArrayOut(BSTR, aBstrArray))
{
    com::SafeArray<BSTR> aBstr(v.size());

    std::vector<std::string>::const_iterator it;

    int i = 0;
    it = v.begin();
    for (; it != v.end(); ++it, ++i)
        Utf8Str(it->c_str()).cloneTo(&aBstr[i]);

    aBstr.detachTo(ComSafeArrayOutArg(aBstrArray));
}

struct HostDnsMonitor::Data
{
    std::vector<PCHostDnsMonitorProxy> proxies;
    HostDnsInformation info;
};

struct HostDnsMonitorProxy::Data
{
    Data(const HostDnsMonitor *aMonitor, const VirtualBox *aParent)
      : info(NULL)
      , virtualbox(aParent)
      , monitor(aMonitor)
      , fModified(true)
    {}

    virtual ~Data()
    {
        if (info)
        {
            delete info;
            info = NULL;
        }
    }

    HostDnsInformation *info;
    const VirtualBox *virtualbox;
    const HostDnsMonitor *monitor;
    bool fModified;
};


HostDnsMonitor::HostDnsMonitor()
  : m(NULL)
{
}

HostDnsMonitor::~HostDnsMonitor()
{
    if (m)
    {
        delete m;
        m = NULL;
    }
}

const HostDnsMonitor *HostDnsMonitor::getHostDnsMonitor()
{
    /* XXX: Moved initialization from HostImpl.cpp */
    if (!g_monitor)
    {
# if defined (RT_OS_DARWIN)
        g_monitor = new HostDnsServiceDarwin();
# elif defined(RT_OS_WINDOWS)
        g_monitor = new HostDnsServiceWin();
# elif defined(RT_OS_LINUX)
        g_monitor = new HostDnsServiceLinux();
# elif defined(RT_OS_SOLARIS)
        g_monitor =  new HostDnsServiceSolaris();
# elif defined(RT_OS_OS2)
        g_monitor = new HostDnsServiceOs2();
# else
        g_monitor = new HostDnsService();
# endif
        g_monitor->init();
    }

    return g_monitor;
}

void HostDnsMonitor::addMonitorProxy(PCHostDnsMonitorProxy proxy) const
{
    ALock l(this);
    m->proxies.push_back(proxy);
    proxy->notify();
}

void HostDnsMonitor::releaseMonitorProxy(PCHostDnsMonitorProxy proxy) const
{
    ALock l(this);
    std::vector<PCHostDnsMonitorProxy>::iterator it;
    it = std::find(m->proxies.begin(), m->proxies.end(), proxy);

    if (it == m->proxies.end())
        return;

    m->proxies.erase(it);
}

void HostDnsMonitor::shutdown()
{
    if (g_monitor)
    {
        delete g_monitor;
        g_monitor = NULL;
    }
}

const HostDnsInformation &HostDnsMonitor::getInfo() const
{
    return m->info;
}

void HostDnsMonitor::notifyAll() const
{
    ALock l(this);
    std::vector<PCHostDnsMonitorProxy>::const_iterator it;
    for (it = m->proxies.begin(); it != m->proxies.end(); ++it)
        (*it)->notify();
}

void HostDnsMonitor::setInfo(const HostDnsInformation &info)
{
    ALock l(this);
    m->info = info;
}

HRESULT HostDnsMonitor::init()
{
    m = new HostDnsMonitor::Data();
    return S_OK;
}


/* HostDnsMonitorProxy */
HostDnsMonitorProxy::HostDnsMonitorProxy()
  : m(NULL)
{
}

HostDnsMonitorProxy::~HostDnsMonitorProxy()
{
    if (m)
    {
        if (m->monitor)
            m->monitor->releaseMonitorProxy(this);
        delete m;
        m = NULL;
    }
}

void HostDnsMonitorProxy::init(const HostDnsMonitor *mon, const VirtualBox* aParent)
{
    m = new HostDnsMonitorProxy::Data(mon, aParent);
    m->monitor->addMonitorProxy(this);
    updateInfo();
}

void HostDnsMonitorProxy::notify() const
{
    m->fModified = true;
    const_cast<VirtualBox *>(m->virtualbox)->onHostNameResolutionConfigurationChange();
}

HRESULT HostDnsMonitorProxy::GetNameServers(ComSafeArrayOut(BSTR, aNameServers))
{
    AssertReturn(m && m->info, E_FAIL);
    ALock l(this);

    if (m->fModified)
        updateInfo();

    detachVectorOfString(m->info->servers, ComSafeArrayOutArg(aNameServers));

    return S_OK;
}

HRESULT HostDnsMonitorProxy::GetDomainName(BSTR *aDomainName)
{
    AssertReturn(m && m->info, E_FAIL);
    ALock l(this);

    if (m->fModified)
        updateInfo();

    Utf8Str(m->info->domain.c_str()).cloneTo(aDomainName);

    return S_OK;
}

HRESULT HostDnsMonitorProxy::GetSearchStrings(ComSafeArrayOut(BSTR, aSearchStrings))
{
    AssertReturn(m && m->info, E_FAIL);
    ALock l(this);

    if (m->fModified)
        updateInfo();

    detachVectorOfString(m->info->searchList, ComSafeArrayOutArg(aSearchStrings));

    return S_OK;
}

bool HostDnsMonitorProxy::operator==(PCHostDnsMonitorProxy& rhs)
{
    if (!m || !rhs->m)
        return false;

    /**
     * we've assigned to the same instance of VirtualBox.
     */
    return m->virtualbox == rhs->m->virtualbox;
}

void HostDnsMonitorProxy::updateInfo()
{
    HostDnsInformation *info = new HostDnsInformation(m->monitor->getInfo());
    HostDnsInformation *old = m->info;

    m->info = info;
    if (old)
        delete old;

    m->fModified = false;
}
