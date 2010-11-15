/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#include "../VBoxVideo-win.h"
#include "../Helper.h"

#ifndef VBOXVHWA_WITH_SHGSMI
# include <iprt/semaphore.h>
# include <iprt/asm.h>
#endif

#define VBOXVHWA_PRIMARY_ALLOCATION(_pSrc) ((_pSrc)->pPrimaryAllocation)


DECLINLINE(void) vboxVhwaHdrInit(VBOXVHWACMD* pHdr, D3DDDI_VIDEO_PRESENT_SOURCE_ID srcId, VBOXVHWACMD_TYPE enmCmd)
{
    memset(pHdr, 0, sizeof(VBOXVHWACMD));
    pHdr->iDisplay = srcId;
    pHdr->rc = VERR_GENERAL_FAILURE;
    pHdr->enmCmd = enmCmd;
#ifndef VBOXVHWA_WITH_SHGSMI
    pHdr->cRefs = 1;
#endif
}

#ifdef VBOXVHWA_WITH_SHGSMI
static int vboxVhwaCommandSubmitHgsmi(struct _DEVICE_EXTENSION* pDevExt, HGSMIOFFSET offDr)
{
    VBoxHGSMIGuestWrite(pDevExt, offDr);
    return VINF_SUCCESS;
}
#else
DECLINLINE(void) vbvaVhwaCommandRelease(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD* pCmd)
{
    uint32_t cRefs = ASMAtomicDecU32(&pCmd->cRefs);
    Assert(cRefs < UINT32_MAX / 2);
    if(!cRefs)
    {
        vboxHGSMIBufferFree(commonFromDeviceExt(pDevExt), pCmd);
    }
}

DECLINLINE(void) vbvaVhwaCommandRetain(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD* pCmd)
{
    ASMAtomicIncU32(&pCmd->cRefs);
}

/* do not wait for completion */
void vboxVhwaCommandSubmitAsynch(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD* pCmd, PFNVBOXVHWACMDCOMPLETION pfnCompletion, void * pContext)
{
    pCmd->GuestVBVAReserved1 = (uintptr_t)pfnCompletion;
    pCmd->GuestVBVAReserved2 = (uintptr_t)pContext;
    vbvaVhwaCommandRetain(pDevExt, pCmd);

    vboxHGSMIBufferSubmit(commonFromDeviceExt(pDevExt), pCmd);

    if(!(pCmd->Flags & VBOXVHWACMD_FLAG_HG_ASYNCH)
            || ((pCmd->Flags & VBOXVHWACMD_FLAG_GH_ASYNCH_NOCOMPLETION)
                    && (pCmd->Flags & VBOXVHWACMD_FLAG_HG_ASYNCH_RETURNED)))
    {
        /* the command is completed */
        pfnCompletion(pDevExt, pCmd, pContext);
    }

    vbvaVhwaCommandRelease(pDevExt, pCmd);
}

static DECLCALLBACK(void) vboxVhwaCompletionSetEvent(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD * pCmd, void * pvContext)
{
    RTSemEventSignal((RTSEMEVENT)pvContext);
}

void vboxVhwaCommandSubmitAsynchByEvent(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD* pCmd, RTSEMEVENT hEvent)
{
    vboxVhwaCommandSubmitAsynch(pDevExt, pCmd, vboxVhwaCompletionSetEvent, hEvent);
}
#endif

void vboxVhwaCommandCheckCompletion(PDEVICE_EXTENSION pDevExt)
{
    NTSTATUS Status = vboxWddmCallIsr(pDevExt);
    Assert(Status == STATUS_SUCCESS);
}

VBOXVHWACMD* vboxVhwaCommandCreate(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID srcId, VBOXVHWACMD_TYPE enmCmd, VBOXVHWACMD_LENGTH cbCmd)
{
    vboxVhwaCommandCheckCompletion(pDevExt);
#ifdef VBOXVHWA_WITH_SHGSMI
    VBOXVHWACMD* pHdr = (VBOXVHWACMD*)VBoxSHGSMICommandAlloc(&pDevExt->u.primary.hgsmiAdapterHeap,
                              cbCmd + VBOXVHWACMD_HEADSIZE(),
                              HGSMI_CH_VBVA,
                              VBVA_VHWA_CMD);
#else
    VBOXVHWACMD* pHdr = (VBOXVHWACMD*)vboxHGSMIBufferAlloc(commonFromDeviceExt(pDevExt),
                              cbCmd + VBOXVHWACMD_HEADSIZE(),
                              HGSMI_CH_VBVA,
                              VBVA_VHWA_CMD);
#endif
    Assert(pHdr);
    if (!pHdr)
    {
        drprintf((__FUNCTION__": vboxHGSMIBufferAlloc failed\n"));
    }
    else
    {
        vboxVhwaHdrInit(pHdr, srcId, enmCmd);
    }

    return pHdr;
}

void vboxVhwaCommandFree(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD* pCmd)
{
#ifdef VBOXVHWA_WITH_SHGSMI
    VBoxSHGSMICommandFree(&pDevExt->u.primary.hgsmiAdapterHeap, pCmd);
#else
    vbvaVhwaCommandRelease(pDevExt, pCmd);
#endif
}

int vboxVhwaCommandSubmit(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD* pCmd)
{
#ifdef VBOXVHWA_WITH_SHGSMI
    const VBOXSHGSMIHEADER* pHdr = VBoxSHGSMICommandPrepSynch(&pDevExt->u.primary.hgsmiAdapterHeap, pCmd);
    Assert(pHdr);
    int rc = VERR_GENERAL_FAILURE;
    if (pHdr)
    {
        do
        {
            HGSMIOFFSET offCmd = VBoxSHGSMICommandOffset(&pDevExt->u.primary.hgsmiAdapterHeap, pHdr);
            Assert(offCmd != HGSMIOFFSET_VOID);
            if (offCmd != HGSMIOFFSET_VOID)
            {
                rc = vboxVhwaCommandSubmitHgsmi(pDevExt, offCmd);
                AssertRC(rc);
                if (RT_SUCCESS(rc))
                {
                    VBoxSHGSMICommandDoneSynch(&pDevExt->u.primary.hgsmiAdapterHeap, pHdr);
                    AssertRC(rc);
                    break;
                }
            }
            else
                rc = VERR_INVALID_PARAMETER;
            /* fail to submit, cancel it */
            VBoxSHGSMICommandCancelSynch(&pDevExt->u.primary.hgsmiAdapterHeap, pHdr);
        } while (0);
    }
    else
        rc = VERR_INVALID_PARAMETER;
    return rc;
#else
    RTSEMEVENT hEvent;
    int rc = RTSemEventCreate(&hEvent);
    AssertRC(rc);
    if (RT_SUCCESS(rc))
    {
        pCmd->Flags |= VBOXVHWACMD_FLAG_GH_ASYNCH_IRQ;
        vboxVhwaCommandSubmitAsynchByEvent(pDevExt, pCmd, hEvent);
        rc = RTSemEventWait(hEvent, RT_INDEFINITE_WAIT);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
            RTSemEventDestroy(hEvent);
    }
    return rc;
#endif
}

#ifndef VBOXVHWA_WITH_SHGSMI
static DECLCALLBACK(void) vboxVhwaCompletionFreeCmd(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD * pCmd, void * pContext)
{
    vboxVhwaCommandFree(pDevExt, pCmd);
}

void vboxVhwaCompletionListProcess(PDEVICE_EXTENSION pDevExt, VBOXSHGSMILIST *pList)
{
    PVBOXSHGSMILIST_ENTRY pNext, pCur;
    for (pCur = pList->pFirst; pCur; pCur = pNext)
    {
        /* need to save next since the command may be released in a pfnCallback and thus its data might be invalid */
        pNext = pCur->pNext;
        VBOXVHWACMD *pCmd = VBOXVHWA_LISTENTRY2CMD(pCur);
        PFNVBOXVHWACMDCOMPLETION pfnCallback = (PFNVBOXVHWACMDCOMPLETION)pCmd->GuestVBVAReserved1;
        void *pvCallback = (void*)pCmd->GuestVBVAReserved2;
        pfnCallback(pDevExt, pCmd, pvCallback);
    }
}

#endif

void vboxVhwaCommandSubmitAsynchAndComplete(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD* pCmd)
{
#ifdef VBOXVHWA_WITH_SHGSMI
# error "port me"
#else
    pCmd->Flags |= VBOXVHWACMD_FLAG_GH_ASYNCH_NOCOMPLETION;

    vboxVhwaCommandSubmitAsynch(pDevExt, pCmd, vboxVhwaCompletionFreeCmd, NULL);
#endif
}

void vboxVhwaFreeHostInfo1(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD_QUERYINFO1* pInfo)
{
    VBOXVHWACMD* pCmd = VBOXVHWACMD_HEAD(pInfo);
    vboxVhwaCommandFree(pDevExt, pCmd);
}

void vboxVhwaFreeHostInfo2(PDEVICE_EXTENSION pDevExt, VBOXVHWACMD_QUERYINFO2* pInfo)
{
    VBOXVHWACMD* pCmd = VBOXVHWACMD_HEAD(pInfo);
    vboxVhwaCommandFree(pDevExt, pCmd);
}

VBOXVHWACMD_QUERYINFO1* vboxVhwaQueryHostInfo1(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID srcId)
{
    VBOXVHWACMD* pCmd = vboxVhwaCommandCreate(pDevExt, srcId, VBOXVHWACMD_TYPE_QUERY_INFO1, sizeof(VBOXVHWACMD_QUERYINFO1));
    VBOXVHWACMD_QUERYINFO1 *pInfo1;

    Assert(pCmd);
    if (!pCmd)
    {
        drprintf((0, "VBoxDISP::vboxVhwaQueryHostInfo1: vboxVhwaCommandCreate failed\n"));
        return NULL;
    }

    pInfo1 = VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_QUERYINFO1);
    pInfo1->u.in.guestVersion.maj = VBOXVHWA_VERSION_MAJ;
    pInfo1->u.in.guestVersion.min = VBOXVHWA_VERSION_MIN;
    pInfo1->u.in.guestVersion.bld = VBOXVHWA_VERSION_BLD;
    pInfo1->u.in.guestVersion.reserved = VBOXVHWA_VERSION_RSV;

    int rc = vboxVhwaCommandSubmit(pDevExt, pCmd);
    AssertRC(rc);
    if(RT_SUCCESS(rc))
    {
        if(RT_SUCCESS(pCmd->rc))
        {
            return VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_QUERYINFO1);
        }
    }

    vboxVhwaCommandFree(pDevExt, pCmd);
    return NULL;
}

VBOXVHWACMD_QUERYINFO2* vboxVhwaQueryHostInfo2(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID srcId, uint32_t numFourCC)
{
    VBOXVHWACMD* pCmd = vboxVhwaCommandCreate(pDevExt, srcId, VBOXVHWACMD_TYPE_QUERY_INFO2, VBOXVHWAINFO2_SIZE(numFourCC));
    VBOXVHWACMD_QUERYINFO2 *pInfo2;
    Assert(pCmd);
    if (!pCmd)
    {
        drprintf((0, "VBoxDISP::vboxVhwaQueryHostInfo2: vboxVhwaCommandCreate failed\n"));
        return NULL;
    }

    pInfo2 = VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_QUERYINFO2);
    pInfo2->numFourCC = numFourCC;

    int rc = vboxVhwaCommandSubmit(pDevExt, pCmd);
    AssertRC(rc);
    if(RT_SUCCESS(rc))
    {
        AssertRC(pCmd->rc);
        if(RT_SUCCESS(pCmd->rc))
        {
            if(pInfo2->numFourCC == numFourCC)
            {
                return pInfo2;
            }
        }
    }

    vboxVhwaCommandFree(pDevExt, pCmd);
    return NULL;
}

int vboxVhwaEnable(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID srcId)
{
    int rc = VERR_GENERAL_FAILURE;
    VBOXVHWACMD* pCmd;

    pCmd = vboxVhwaCommandCreate(pDevExt, srcId, VBOXVHWACMD_TYPE_ENABLE, 0);
    Assert(pCmd);
    if (!pCmd)
    {
        drprintf((0, "VBoxDISP::vboxVhwaEnable: vboxVhwaCommandCreate failed\n"));
        return rc;
    }

    rc = vboxVhwaCommandSubmit(pDevExt, pCmd);
    AssertRC(rc);
    if(RT_SUCCESS(rc))
    {
        AssertRC(pCmd->rc);
        if(RT_SUCCESS(pCmd->rc))
            rc = VINF_SUCCESS;
        else
            rc = pCmd->rc;
    }

    vboxVhwaCommandFree(pDevExt, pCmd);
    return rc;
}

int vboxVhwaDisable(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID srcId)
{
    vboxVhwaCommandCheckCompletion(pDevExt);

    int rc = VERR_GENERAL_FAILURE;
    VBOXVHWACMD* pCmd;

    pCmd = vboxVhwaCommandCreate(pDevExt, srcId, VBOXVHWACMD_TYPE_DISABLE, 0);
    Assert(pCmd);
    if (!pCmd)
    {
        drprintf((0, "VBoxDISP::vboxVhwaDisable: vboxVhwaCommandCreate failed\n"));
        return rc;
    }

    rc = vboxVhwaCommandSubmit(pDevExt, pCmd);
    AssertRC(rc);
    if(RT_SUCCESS(rc))
    {
        AssertRC(pCmd->rc);
        if(RT_SUCCESS(pCmd->rc))
            rc = VINF_SUCCESS;
        else
            rc = pCmd->rc;
    }

    vboxVhwaCommandFree(pDevExt, pCmd);
    return rc;
}

DECLINLINE(VOID) vboxVhwaHlpOverlayListInit(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[VidPnSourceId];
    pSource->cOverlays = 0;
    InitializeListHead(&pSource->OverlayList);
    KeInitializeSpinLock(&pSource->OverlayListLock);
}

static void vboxVhwaInitSrc(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID srcId)
{
    Assert(srcId < (D3DDDI_VIDEO_PRESENT_SOURCE_ID)commonFromDeviceExt(pDevExt)->cDisplays);
    VBOXVHWA_INFO *pSettings = &pDevExt->aSources[srcId].Vhwa.Settings;
    memset (pSettings, 0, sizeof (VBOXVHWA_INFO));

    vboxVhwaHlpOverlayListInit(pDevExt, srcId);

    VBOXVHWACMD_QUERYINFO1* pInfo1 = vboxVhwaQueryHostInfo1(pDevExt, srcId);
    if (pInfo1)
    {
        if ((pInfo1->u.out.cfgFlags & VBOXVHWA_CFG_ENABLED)
                && pInfo1->u.out.numOverlays)
        {
            if ((pInfo1->u.out.caps & VBOXVHWA_CAPS_OVERLAY)
                    && (pInfo1->u.out.caps & VBOXVHWA_CAPS_OVERLAYSTRETCH)
                    && (pInfo1->u.out.surfaceCaps & VBOXVHWA_SCAPS_OVERLAY)
                    && (pInfo1->u.out.surfaceCaps & VBOXVHWA_SCAPS_FLIP)
                    && (pInfo1->u.out.surfaceCaps & VBOXVHWA_SCAPS_LOCALVIDMEM)
                    && pInfo1->u.out.numOverlays)
            {
                pSettings->fFlags |= VBOXVHWA_F_ENABLED;

                if (pInfo1->u.out.caps & VBOXVHWA_CAPS_COLORKEY)
                {
                    if (pInfo1->u.out.colorKeyCaps & VBOXVHWA_CKEYCAPS_SRCOVERLAY)
                    {
                        pSettings->fFlags |= VBOXVHWA_F_CKEY_SRC;
                        /* todo: VBOXVHWA_CKEYCAPS_SRCOVERLAYONEACTIVE ? */
                    }

                    if (pInfo1->u.out.colorKeyCaps & VBOXVHWA_CKEYCAPS_DESTOVERLAY)
                    {
                        pSettings->fFlags |= VBOXVHWA_F_CKEY_DST;
                        /* todo: VBOXVHWA_CKEYCAPS_DESTOVERLAYONEACTIVE ? */
                    }
                }

                pSettings->cOverlaysSupported = pInfo1->u.out.numOverlays;

                pSettings->cFormats = 0;

                pSettings->aFormats[pSettings->cFormats] = D3DDDIFMT_X8R8G8B8;
                ++pSettings->cFormats;

                if (pInfo1->u.out.numFourCC
                        && (pInfo1->u.out.caps & VBOXVHWA_CAPS_OVERLAYFOURCC))
                {
                    VBOXVHWACMD_QUERYINFO2* pInfo2 = vboxVhwaQueryHostInfo2(pDevExt, srcId, pInfo1->u.out.numFourCC);
                    if (pInfo2)
                    {
                        for (uint32_t i = 0; i < pInfo2->numFourCC; ++i)
                        {
                            pSettings->aFormats[pSettings->cFormats] = (D3DDDIFORMAT)pInfo2->FourCC[i];
                            ++pSettings->cFormats;
                        }
                        vboxVhwaFreeHostInfo2(pDevExt, pInfo2);
                    }
                }
            }
        }
        vboxVhwaFreeHostInfo1(pDevExt, pInfo1);
    }
}

void vboxVhwaInit(PDEVICE_EXTENSION pDevExt)
{
    for (int i = 0; i < commonFromDeviceExt(pDevExt)->cDisplays; ++i)
    {
        vboxVhwaInitSrc(pDevExt, (D3DDDI_VIDEO_PRESENT_SOURCE_ID)i);
    }
}

void vboxVhwaFree(PDEVICE_EXTENSION pDevExt)
{
    /* we do not allocate/map anything, just issue a Disable command
     * to ensure all pending commands are flushed */
    for (int i = 0; i < commonFromDeviceExt(pDevExt)->cDisplays; ++i)
    {
        vboxVhwaDisable(pDevExt, i);
    }
}

int vboxVhwaHlpTranslateFormat(VBOXVHWA_PIXELFORMAT *pFormat, D3DDDIFORMAT enmFormat)
{
    pFormat->Reserved = 0;
    switch (enmFormat)
    {
        case D3DDDIFMT_A8R8G8B8:
        case D3DDDIFMT_X8R8G8B8:
            pFormat->flags = VBOXVHWA_PF_RGB;
            pFormat->c.rgbBitCount = 32;
            pFormat->m1.rgbRBitMask = 0xff0000;
            pFormat->m2.rgbGBitMask = 0xff00;
            pFormat->m3.rgbBBitMask = 0xff;
            /* always zero for now */
            pFormat->m4.rgbABitMask = 0;
            return VINF_SUCCESS;
        case D3DDDIFMT_R8G8B8:
            pFormat->flags = VBOXVHWA_PF_RGB;
            pFormat->c.rgbBitCount = 24;
            pFormat->m1.rgbRBitMask = 0xff0000;
            pFormat->m2.rgbGBitMask = 0xff00;
            pFormat->m3.rgbBBitMask = 0xff;
            /* always zero for now */
            pFormat->m4.rgbABitMask = 0;
            return VINF_SUCCESS;
        case D3DDDIFMT_R5G6B5:
            pFormat->flags = VBOXVHWA_PF_RGB;
            pFormat->c.rgbBitCount = 16;
            pFormat->m1.rgbRBitMask = 0xf800;
            pFormat->m2.rgbGBitMask = 0x7e0;
            pFormat->m3.rgbBBitMask = 0x1f;
            /* always zero for now */
            pFormat->m4.rgbABitMask = 0;
            return VINF_SUCCESS;
        case D3DDDIFMT_P8:
        case D3DDDIFMT_A8:
        case D3DDDIFMT_X1R5G5B5:
        case D3DDDIFMT_A1R5G5B5:
        case D3DDDIFMT_A4R4G4B4:
        case D3DDDIFMT_R3G3B2:
        case D3DDDIFMT_A8R3G3B2:
        case D3DDDIFMT_X4R4G4B4:
        case D3DDDIFMT_A2B10G10R10:
        case D3DDDIFMT_A8B8G8R8:
        case D3DDDIFMT_X8B8G8R8:
        case D3DDDIFMT_G16R16:
        case D3DDDIFMT_A2R10G10B10:
        case D3DDDIFMT_A16B16G16R16:
        case D3DDDIFMT_A8P8:
        default:
        {
            uint32_t fourcc = vboxWddmFormatToFourcc(enmFormat);
            Assert(fourcc);
            if (fourcc)
            {
                pFormat->flags = VBOXVHWA_PF_FOURCC;
                pFormat->fourCC = fourcc;
                return VINF_SUCCESS;
            }
            return VERR_NOT_SUPPORTED;
        }
    }
}

int vboxVhwaHlpDestroySurface(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_ALLOCATION pSurf,
        D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    Assert(pSurf->hHostHandle);
    if (!pSurf->hHostHandle)
        return VERR_INVALID_STATE;

    VBOXVHWACMD* pCmd = vboxVhwaCommandCreate(pDevExt, VidPnSourceId,
            VBOXVHWACMD_TYPE_SURF_DESTROY, sizeof(VBOXVHWACMD_SURF_DESTROY));
    Assert(pCmd);
    if(pCmd)
    {
        VBOXVHWACMD_SURF_DESTROY * pBody = VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_SURF_DESTROY);

        memset(pBody, 0, sizeof(VBOXVHWACMD_SURF_DESTROY));

        pBody->u.in.hSurf = pSurf->hHostHandle;

        /* we're not interested in completion, just send the command */
        vboxVhwaCommandSubmitAsynchAndComplete(pDevExt, pCmd);

        pSurf->hHostHandle = VBOXVHWA_SURFHANDLE_INVALID;

        return VINF_SUCCESS;
    }

    return VERR_OUT_OF_RESOURCES;
}

int vboxVhwaHlpPopulateSurInfo(VBOXVHWA_SURFACEDESC *pInfo, PVBOXWDDM_ALLOCATION pSurf,
        uint32_t fFlags, uint32_t cBackBuffers, uint32_t fSCaps,
        D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    memset(pInfo, 0, sizeof(VBOXVHWA_SURFACEDESC));

    pInfo->height = pSurf->SurfDesc.height;
    pInfo->width = pSurf->SurfDesc.width;
    pInfo->flags |= VBOXVHWA_SD_HEIGHT | VBOXVHWA_SD_WIDTH;
    if (fFlags & VBOXVHWA_SD_PITCH)
    {
        pInfo->pitch = pSurf->SurfDesc.pitch;
        pInfo->flags |= VBOXVHWA_SD_PITCH;
        pInfo->sizeX = pSurf->SurfDesc.cbSize;
        pInfo->sizeY = 1;
    }

    if (cBackBuffers)
    {
        pInfo->cBackBuffers = cBackBuffers;
        pInfo->flags |= VBOXVHWA_SD_BACKBUFFERCOUNT;
    }
    else
        pInfo->cBackBuffers = 0;
    pInfo->Reserved = 0;
        /* @todo: color keys */
//                        pInfo->DstOverlayCK;
//                        pInfo->DstBltCK;
//                        pInfo->SrcOverlayCK;
//                        pInfo->SrcBltCK;
    int rc = vboxVhwaHlpTranslateFormat(&pInfo->PixelFormat, pSurf->SurfDesc.format);
    AssertRC(rc);
    if (RT_SUCCESS(rc))
    {
        pInfo->flags |= VBOXVHWA_SD_PIXELFORMAT;
        pInfo->surfCaps = fSCaps;
        pInfo->flags |= VBOXVHWA_SD_CAPS;
        pInfo->offSurface = pSurf->offVram;
    }

    return rc;
}

int vboxVhwaHlpCheckApplySurfInfo(PVBOXWDDM_ALLOCATION pSurf, VBOXVHWA_SURFACEDESC *pInfo,
        uint32_t fFlags, bool bApplyHostHandle)
{
    int rc = VINF_SUCCESS;
    if (!(fFlags & VBOXVHWA_SD_PITCH))
    {
        /* should be set by host */
//        Assert(pInfo->flags & VBOXVHWA_SD_PITCH);
        pSurf->SurfDesc.cbSize = pInfo->sizeX * pInfo->sizeY;
        Assert(pSurf->SurfDesc.cbSize);
        pSurf->SurfDesc.pitch = pInfo->pitch;
        Assert(pSurf->SurfDesc.pitch);
        /* @todo: make this properly */
        pSurf->SurfDesc.bpp = pSurf->SurfDesc.pitch * 8 / pSurf->SurfDesc.width;
        Assert(pSurf->SurfDesc.bpp);
    }
    else
    {
        Assert(pSurf->SurfDesc.cbSize ==  pInfo->sizeX);
        Assert(pInfo->sizeY == 1);
        Assert(pInfo->pitch == pSurf->SurfDesc.pitch);
        if (pSurf->SurfDesc.cbSize !=  pInfo->sizeX
                || pInfo->sizeY != 1
                || pInfo->pitch != pSurf->SurfDesc.pitch)
        {
            rc = VERR_INVALID_PARAMETER;
        }
    }

    if (bApplyHostHandle && RT_SUCCESS(rc))
    {
        pSurf->hHostHandle = pInfo->hSurf;
    }

    return rc;
}

int vboxVhwaHlpCreateSurface(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_ALLOCATION pSurf,
        uint32_t fFlags, uint32_t cBackBuffers, uint32_t fSCaps,
        D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    /* the first thing we need is to post create primary */
    VBOXVHWACMD* pCmd = vboxVhwaCommandCreate(pDevExt, VidPnSourceId,
                VBOXVHWACMD_TYPE_SURF_CREATE, sizeof(VBOXVHWACMD_SURF_CREATE));
    Assert(pCmd);
    if (pCmd)
    {
        VBOXVHWACMD_SURF_CREATE * pBody = VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_SURF_CREATE);
        int rc = VINF_SUCCESS;

        memset(pBody, 0, sizeof(VBOXVHWACMD_SURF_CREATE));

        rc = vboxVhwaHlpPopulateSurInfo(&pBody->SurfInfo, pSurf,
                fFlags, cBackBuffers, fSCaps,
                VidPnSourceId);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
        {
            vboxVhwaCommandSubmit(pDevExt, pCmd);
            Assert(pCmd->rc == VINF_SUCCESS);
            if(pCmd->rc == VINF_SUCCESS)
            {
                rc = vboxVhwaHlpCheckApplySurfInfo(pSurf, &pBody->SurfInfo, fFlags, true);
            }
            else
                rc = pCmd->rc;
        }
        vboxVhwaCommandFree(pDevExt, pCmd);
        return rc;
    }

    return VERR_OUT_OF_RESOURCES;
}

int vboxVhwaHlpGetSurfInfoForSource(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_ALLOCATION pSurf, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    /* the first thing we need is to post create primary */
    VBOXVHWACMD* pCmd = vboxVhwaCommandCreate(pDevExt, VidPnSourceId,
            VBOXVHWACMD_TYPE_SURF_GETINFO, sizeof(VBOXVHWACMD_SURF_GETINFO));
    Assert(pCmd);
    if (pCmd)
    {
        VBOXVHWACMD_SURF_GETINFO * pBody = VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_SURF_GETINFO);
        int rc = VINF_SUCCESS;

        memset(pBody, 0, sizeof(VBOXVHWACMD_SURF_GETINFO));

        rc = vboxVhwaHlpPopulateSurInfo(&pBody->SurfInfo, pSurf,
                0, 0, VBOXVHWA_SCAPS_OVERLAY | VBOXVHWA_SCAPS_VIDEOMEMORY | VBOXVHWA_SCAPS_LOCALVIDMEM | VBOXVHWA_SCAPS_COMPLEX,
                VidPnSourceId);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
        {
            vboxVhwaCommandSubmit(pDevExt, pCmd);
            Assert(pCmd->rc == VINF_SUCCESS);
            if(pCmd->rc == VINF_SUCCESS)
            {
                rc = vboxVhwaHlpCheckApplySurfInfo(pSurf, &pBody->SurfInfo, 0, true);
            }
            else
                rc = pCmd->rc;
        }
        vboxVhwaCommandFree(pDevExt, pCmd);
        return rc;
    }

    return VERR_OUT_OF_RESOURCES;
}

int vboxVhwaHlpGetSurfInfo(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_ALLOCATION pSurf)
{
    for (int i = 0; i < commonFromDeviceExt(pDevExt)->cDisplays; ++i)
    {
        PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[i];
        if (pSource->Vhwa.Settings.fFlags & VBOXVHWA_F_ENABLED)
        {
            int rc = vboxVhwaHlpGetSurfInfoForSource(pDevExt, pSurf, i);
            AssertRC(rc);
            return rc;
        }
    }
    AssertBreakpoint();
    return VERR_NOT_SUPPORTED;
}

int vboxVhwaHlpDestroyPrimary(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_SOURCE pSource, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    PVBOXWDDM_ALLOCATION pFbSurf = VBOXVHWA_PRIMARY_ALLOCATION(pSource);

    int rc = vboxVhwaHlpDestroySurface(pDevExt, pFbSurf, VidPnSourceId);
    AssertRC(rc);
    return rc;
}

int vboxVhwaHlpCreatePrimary(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_SOURCE pSource, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    PVBOXWDDM_ALLOCATION pFbSurf = VBOXVHWA_PRIMARY_ALLOCATION(pSource);
    Assert(pSource->Vhwa.cOverlaysCreated == 1);
    Assert(pFbSurf->hHostHandle == VBOXVHWA_SURFHANDLE_INVALID);
    if (pFbSurf->hHostHandle != VBOXVHWA_SURFHANDLE_INVALID)
        return VERR_INVALID_STATE;

    int rc = vboxVhwaHlpCreateSurface(pDevExt, pFbSurf,
            VBOXVHWA_SD_PITCH, 0, VBOXVHWA_SCAPS_PRIMARYSURFACE | VBOXVHWA_SCAPS_VIDEOMEMORY | VBOXVHWA_SCAPS_LOCALVIDMEM,
            VidPnSourceId);
    AssertRC(rc);
    return rc;
}

int vboxVhwaHlpCheckInit(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    Assert(VidPnSourceId < (D3DDDI_VIDEO_PRESENT_SOURCE_ID)commonFromDeviceExt(pDevExt)->cDisplays);
    if (VidPnSourceId >= (D3DDDI_VIDEO_PRESENT_SOURCE_ID)commonFromDeviceExt(pDevExt)->cDisplays)
        return VERR_INVALID_PARAMETER;

    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[VidPnSourceId];

    Assert(!!(pSource->Vhwa.Settings.fFlags & VBOXVHWA_F_ENABLED));
    if (!(pSource->Vhwa.Settings.fFlags & VBOXVHWA_F_ENABLED))
        return VERR_NOT_SUPPORTED;

    int rc = VINF_SUCCESS;
    /* @todo: need a better sync */
    uint32_t cNew = ASMAtomicIncU32(&pSource->Vhwa.cOverlaysCreated);
    if (cNew == 1)
    {
        rc = vboxVhwaEnable(pDevExt, VidPnSourceId);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
        {
            rc = vboxVhwaHlpCreatePrimary(pDevExt, pSource, VidPnSourceId);
            AssertRC(rc);
            if (RT_FAILURE(rc))
            {
                int tmpRc = vboxVhwaDisable(pDevExt, VidPnSourceId);
                AssertRC(tmpRc);
            }
        }
    }
    else
    {
        PVBOXWDDM_ALLOCATION pFbSurf = VBOXVHWA_PRIMARY_ALLOCATION(pSource);
        Assert(pFbSurf->hHostHandle);
        if (pFbSurf->hHostHandle)
            rc = VINF_ALREADY_INITIALIZED;
        else
            rc = VERR_INVALID_STATE;
    }

    if (RT_FAILURE(rc))
        ASMAtomicDecU32(&pSource->Vhwa.cOverlaysCreated);

    return rc;
}

int vboxVhwaHlpCheckTerm(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    Assert(VidPnSourceId < (D3DDDI_VIDEO_PRESENT_SOURCE_ID)commonFromDeviceExt(pDevExt)->cDisplays);
    if (VidPnSourceId >= (D3DDDI_VIDEO_PRESENT_SOURCE_ID)commonFromDeviceExt(pDevExt)->cDisplays)
        return VERR_INVALID_PARAMETER;

    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[VidPnSourceId];

    Assert(!!(pSource->Vhwa.Settings.fFlags & VBOXVHWA_F_ENABLED));

    /* @todo: need a better sync */
    uint32_t cNew = ASMAtomicDecU32(&pSource->Vhwa.cOverlaysCreated);
    int rc = VINF_SUCCESS;
    if (!cNew)
    {
        rc = vboxVhwaHlpDestroyPrimary(pDevExt, pSource, VidPnSourceId);
        AssertRC(rc);
    }
    else
    {
        Assert(cNew < UINT32_MAX / 2);
    }

    return rc;
}

int vboxVhwaHlpOverlayFlip(PVBOXWDDM_OVERLAY pOverlay, const DXGKARG_FLIPOVERLAY *pFlipInfo)
{
    PVBOXWDDM_ALLOCATION pAlloc = (PVBOXWDDM_ALLOCATION)pFlipInfo->hSource;
    Assert(pAlloc->hHostHandle);
    Assert(pAlloc->pResource);
    Assert(pAlloc->pResource == pOverlay->pResource);
    Assert(pFlipInfo->PrivateDriverDataSize == sizeof (VBOXWDDM_OVERLAYFLIP_INFO));
    Assert(pFlipInfo->pPrivateDriverData);
    PVBOXWDDM_SOURCE pSource = &pOverlay->pDevExt->aSources[pOverlay->VidPnSourceId];
    Assert(!!(pSource->Vhwa.Settings.fFlags & VBOXVHWA_F_ENABLED));
    PVBOXWDDM_ALLOCATION pFbSurf = VBOXVHWA_PRIMARY_ALLOCATION(pSource);
    Assert(pFbSurf);
    Assert(pFbSurf->hHostHandle);
    Assert(pFbSurf->offVram != VBOXVIDEOOFFSET_VOID);
    Assert(pOverlay->pCurentAlloc);
    Assert(pOverlay->pCurentAlloc->pResource == pOverlay->pResource);
    Assert(pOverlay->pCurentAlloc != pAlloc);
    int rc = VINF_SUCCESS;
    if (pFlipInfo->PrivateDriverDataSize == sizeof (VBOXWDDM_OVERLAYFLIP_INFO))
    {
        PVBOXWDDM_OVERLAYFLIP_INFO pOurInfo = (PVBOXWDDM_OVERLAYFLIP_INFO)pFlipInfo->pPrivateDriverData;

        VBOXVHWACMD* pCmd = vboxVhwaCommandCreate(pOverlay->pDevExt, pOverlay->VidPnSourceId,
                VBOXVHWACMD_TYPE_SURF_FLIP, sizeof(VBOXVHWACMD_SURF_FLIP));
        Assert(pCmd);
        if(pCmd)
        {
            VBOXVHWACMD_SURF_FLIP * pBody = VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_SURF_FLIP);

            memset(pBody, 0, sizeof(VBOXVHWACMD_SURF_FLIP));

//            pBody->TargGuestSurfInfo;
//            pBody->CurrGuestSurfInfo;
            pBody->u.in.hTargSurf = pAlloc->hHostHandle;
            pBody->u.in.offTargSurface = pFlipInfo->SrcPhysicalAddress.QuadPart;
            pAlloc->offVram = pFlipInfo->SrcPhysicalAddress.QuadPart;
            pBody->u.in.hCurrSurf = pOverlay->pCurentAlloc->hHostHandle;
            pBody->u.in.offCurrSurface = pOverlay->pCurentAlloc->offVram;
            if (pOurInfo->DirtyRegion.fFlags & VBOXWDDM_DIRTYREGION_F_VALID)
            {
                pBody->u.in.xUpdatedTargMemValid = 1;
                if (pOurInfo->DirtyRegion.fFlags & VBOXWDDM_DIRTYREGION_F_RECT_VALID)
                    pBody->u.in.xUpdatedTargMemRect = *(VBOXVHWA_RECTL*)((void*)&pOurInfo->DirtyRegion.Rect);
                else
                {
                    pBody->u.in.xUpdatedTargMemRect.right = pAlloc->SurfDesc.width;
                    pBody->u.in.xUpdatedTargMemRect.bottom = pAlloc->SurfDesc.height;
                    /* top & left are zero-inited with the above memset */
                }
            }

            /* we're not interested in completion, just send the command */
            vboxVhwaCommandSubmitAsynchAndComplete(pOverlay->pDevExt, pCmd);

            pOverlay->pCurentAlloc = pAlloc;

            rc = VINF_SUCCESS;
        }
        else
            rc = VERR_OUT_OF_RESOURCES;
    }
    else
        rc = VERR_INVALID_PARAMETER;

    return rc;
}

AssertCompile(sizeof (RECT) == sizeof (VBOXVHWA_RECTL));
AssertCompile(RT_SIZEOFMEMB(RECT, left) == RT_SIZEOFMEMB(VBOXVHWA_RECTL, left));
AssertCompile(RT_SIZEOFMEMB(RECT, right) == RT_SIZEOFMEMB(VBOXVHWA_RECTL, right));
AssertCompile(RT_SIZEOFMEMB(RECT, top) == RT_SIZEOFMEMB(VBOXVHWA_RECTL, top));
AssertCompile(RT_SIZEOFMEMB(RECT, bottom) == RT_SIZEOFMEMB(VBOXVHWA_RECTL, bottom));
AssertCompile(RT_OFFSETOF(RECT, left) == RT_OFFSETOF(VBOXVHWA_RECTL, left));
AssertCompile(RT_OFFSETOF(RECT, right) == RT_OFFSETOF(VBOXVHWA_RECTL, right));
AssertCompile(RT_OFFSETOF(RECT, top) == RT_OFFSETOF(VBOXVHWA_RECTL, top));
AssertCompile(RT_OFFSETOF(RECT, bottom) == RT_OFFSETOF(VBOXVHWA_RECTL, bottom));

int vboxVhwaHlpColorFill(PVBOXWDDM_OVERLAY pOverlay, PVBOXWDDM_DMA_PRIVATEDATA_CLRFILL pCF)
{
    PVBOXWDDM_ALLOCATION pAlloc = pCF->ClrFill.Alloc.pAlloc;
    Assert(pAlloc->pResource == pOverlay->pResource);
#ifdef VBOXWDDM_RENDER_FROM_SHADOW
    if (pAlloc->bAssigned)
    {
        /* check if this is a primary surf */
        PVBOXWDDM_SOURCE pSource = &pOverlay->pDevExt->aSources[pOverlay->VidPnSourceId];
        if (pSource->pPrimaryAllocation == pAlloc)
        {
            pAlloc = pSource->pShadowAllocation;
            Assert(pAlloc->pResource == pOverlay->pResource);
        }
    }
#endif
    Assert(pAlloc->hHostHandle);
    Assert(pAlloc->pResource);
    Assert(pAlloc->offVram != VBOXVIDEOOFFSET_VOID);

    int rc;
    VBOXVHWACMD* pCmd = vboxVhwaCommandCreate(pOverlay->pDevExt, pOverlay->VidPnSourceId,
                VBOXVHWACMD_TYPE_SURF_FLIP, RT_OFFSETOF(VBOXVHWACMD_SURF_COLORFILL, u.in.aRects[pCF->ClrFill.Rects.cRects]));
    Assert(pCmd);
    if(pCmd)
    {
        VBOXVHWACMD_SURF_COLORFILL * pBody = VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_SURF_COLORFILL);

        memset(pBody, 0, sizeof(VBOXVHWACMD_SURF_COLORFILL));

        pBody->u.in.hSurf = pAlloc->hHostHandle;
        pBody->u.in.offSurface = pAlloc->offVram;
        pBody->u.in.cRects = pCF->ClrFill.Rects.cRects;
        memcpy (pBody->u.in.aRects, pCF->ClrFill.Rects.aRects, pCF->ClrFill.Rects.cRects * sizeof (pCF->ClrFill.Rects.aRects[0]));
        vboxVhwaCommandSubmitAsynchAndComplete(pOverlay->pDevExt, pCmd);

        rc = VINF_SUCCESS;
    }
    else
        rc = VERR_OUT_OF_RESOURCES;

    return rc;
}

static void vboxVhwaHlpOverlayDstRectSet(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_OVERLAY pOverlay, const RECT *pRect)
{
    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[pOverlay->VidPnSourceId];
    KIRQL OldIrql;
    KeAcquireSpinLock(&pSource->OverlayListLock, &OldIrql);
    pOverlay->DstRect = *pRect;
    KeReleaseSpinLock(&pSource->OverlayListLock, OldIrql);
}

static void vboxVhwaHlpOverlayListAdd(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_OVERLAY pOverlay)
{
    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[pOverlay->VidPnSourceId];
    KIRQL OldIrql;
    KeAcquireSpinLock(&pSource->OverlayListLock, &OldIrql);
    ASMAtomicIncU32(&pSource->cOverlays);
    InsertHeadList(&pSource->OverlayList, &pOverlay->ListEntry);
    KeReleaseSpinLock(&pSource->OverlayListLock, OldIrql);
}

static void vboxVhwaHlpOverlayListRemove(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_OVERLAY pOverlay)
{
    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[pOverlay->VidPnSourceId];
    KIRQL OldIrql;
    KeAcquireSpinLock(&pSource->OverlayListLock, &OldIrql);
    ASMAtomicDecU32(&pSource->cOverlays);
    RemoveEntryList(&pOverlay->ListEntry);
    KeReleaseSpinLock(&pSource->OverlayListLock, OldIrql);
}

AssertCompile(sizeof (RECT) == sizeof (VBOXVHWA_RECTL));
AssertCompile(RT_SIZEOFMEMB(RECT, left) == RT_SIZEOFMEMB(VBOXVHWA_RECTL, left));
AssertCompile(RT_SIZEOFMEMB(RECT, right) == RT_SIZEOFMEMB(VBOXVHWA_RECTL, right));
AssertCompile(RT_SIZEOFMEMB(RECT, top) == RT_SIZEOFMEMB(VBOXVHWA_RECTL, top));
AssertCompile(RT_SIZEOFMEMB(RECT, bottom) == RT_SIZEOFMEMB(VBOXVHWA_RECTL, bottom));
AssertCompile(RT_OFFSETOF(RECT, left) == RT_OFFSETOF(VBOXVHWA_RECTL, left));
AssertCompile(RT_OFFSETOF(RECT, right) == RT_OFFSETOF(VBOXVHWA_RECTL, right));
AssertCompile(RT_OFFSETOF(RECT, top) == RT_OFFSETOF(VBOXVHWA_RECTL, top));
AssertCompile(RT_OFFSETOF(RECT, bottom) == RT_OFFSETOF(VBOXVHWA_RECTL, bottom));

int vboxVhwaHlpOverlayUpdate(PVBOXWDDM_OVERLAY pOverlay, const DXGK_OVERLAYINFO *pOverlayInfo, RECT * pDstUpdateRect)
{
    PVBOXWDDM_ALLOCATION pAlloc = (PVBOXWDDM_ALLOCATION)pOverlayInfo->hAllocation;
    Assert(pAlloc->hHostHandle);
    Assert(pAlloc->pResource);
    Assert(pAlloc->pResource == pOverlay->pResource);
    Assert(pOverlayInfo->PrivateDriverDataSize == sizeof (VBOXWDDM_OVERLAY_INFO));
    Assert(pOverlayInfo->pPrivateDriverData);
    PVBOXWDDM_SOURCE pSource = &pOverlay->pDevExt->aSources[pOverlay->VidPnSourceId];
    Assert(!!(pSource->Vhwa.Settings.fFlags & VBOXVHWA_F_ENABLED));
    PVBOXWDDM_ALLOCATION pFbSurf = VBOXVHWA_PRIMARY_ALLOCATION(pSource);
    Assert(pFbSurf);
    Assert(pFbSurf->hHostHandle);
    Assert(pFbSurf->offVram != VBOXVIDEOOFFSET_VOID);
    int rc = VINF_SUCCESS;
    if (pOverlayInfo->PrivateDriverDataSize == sizeof (VBOXWDDM_OVERLAY_INFO))
    {
        PVBOXWDDM_OVERLAY_INFO pOurInfo = (PVBOXWDDM_OVERLAY_INFO)pOverlayInfo->pPrivateDriverData;

        VBOXVHWACMD* pCmd = vboxVhwaCommandCreate(pOverlay->pDevExt, pOverlay->VidPnSourceId,
                VBOXVHWACMD_TYPE_SURF_OVERLAY_UPDATE, sizeof(VBOXVHWACMD_SURF_OVERLAY_UPDATE));
        Assert(pCmd);
        if(pCmd)
        {
            VBOXVHWACMD_SURF_OVERLAY_UPDATE * pBody = VBOXVHWACMD_BODY(pCmd, VBOXVHWACMD_SURF_OVERLAY_UPDATE);

            memset(pBody, 0, sizeof(VBOXVHWACMD_SURF_OVERLAY_UPDATE));

            pBody->u.in.hDstSurf = pFbSurf->hHostHandle;
            pBody->u.in.offDstSurface = pFbSurf->offVram;
            pBody->u.in.dstRect = *(VBOXVHWA_RECTL*)((void*)&pOverlayInfo->DstRect);
            pBody->u.in.hSrcSurf = pAlloc->hHostHandle;
            pBody->u.in.offSrcSurface = pOverlayInfo->PhysicalAddress.QuadPart;
            pAlloc->offVram = pOverlayInfo->PhysicalAddress.QuadPart;
            pBody->u.in.srcRect = *(VBOXVHWA_RECTL*)((void*)&pOverlayInfo->SrcRect);
            pBody->u.in.flags |= VBOXVHWA_OVER_SHOW;
            if (pOurInfo->OverlayDesc.fFlags & VBOXWDDM_OVERLAY_F_CKEY_DST)
            {
                pBody->u.in.flags |= VBOXVHWA_OVER_KEYDESTOVERRIDE /* ?? VBOXVHWA_OVER_KEYDEST */;
                pBody->u.in.desc.DstCK.high = pOurInfo->OverlayDesc.DstColorKeyHigh;
                pBody->u.in.desc.DstCK.low = pOurInfo->OverlayDesc.DstColorKeyLow;
            }

            if (pOurInfo->OverlayDesc.fFlags & VBOXWDDM_OVERLAY_F_CKEY_SRC)
            {
                pBody->u.in.flags |= VBOXVHWA_OVER_KEYSRCOVERRIDE /* ?? VBOXVHWA_OVER_KEYSRC */;
                pBody->u.in.desc.SrcCK.high = pOurInfo->OverlayDesc.SrcColorKeyHigh;
                pBody->u.in.desc.SrcCK.low = pOurInfo->OverlayDesc.SrcColorKeyLow;
            }

            if (pOurInfo->DirtyRegion.fFlags & VBOXWDDM_DIRTYREGION_F_VALID)
            {
                if (pOurInfo->DirtyRegion.fFlags & VBOXWDDM_DIRTYREGION_F_RECT_VALID)
                    pBody->u.in.xUpdatedSrcMemRect = *(VBOXVHWA_RECTL*)((void*)&pOurInfo->DirtyRegion.Rect);
                else
                {
                    pBody->u.in.xUpdatedSrcMemRect.right = pAlloc->SurfDesc.width;
                    pBody->u.in.xUpdatedSrcMemRect.bottom = pAlloc->SurfDesc.height;
                    /* top & left are zero-inited with the above memset */
                }
            }

            if (pDstUpdateRect)
            {
                pBody->u.in.xUpdatedDstMemRect = *(VBOXVHWA_RECTL*)((void*)pDstUpdateRect);
            }

            /* we're not interested in completion, just send the command */
            vboxVhwaCommandSubmitAsynchAndComplete(pOverlay->pDevExt, pCmd);

            pOverlay->pCurentAlloc = pAlloc;

            vboxVhwaHlpOverlayDstRectSet(pOverlay->pDevExt, pOverlay, &pOverlayInfo->DstRect);

            rc = VINF_SUCCESS;
        }
        else
            rc = VERR_OUT_OF_RESOURCES;
    }
    else
        rc = VERR_INVALID_PARAMETER;

    return rc;
}

int vboxVhwaHlpOverlayUpdate(PVBOXWDDM_OVERLAY pOverlay, const DXGK_OVERLAYINFO *pOverlayInfo)
{
    return vboxVhwaHlpOverlayUpdate(pOverlay, pOverlayInfo, NULL);
}

int vboxVhwaHlpOverlayDestroy(PVBOXWDDM_OVERLAY pOverlay)
{
    int rc = VINF_SUCCESS;

    vboxVhwaHlpOverlayListRemove(pOverlay->pDevExt, pOverlay);

    for (uint32_t i = 0; i < pOverlay->pResource->cAllocations; ++i)
    {
        PVBOXWDDM_ALLOCATION pCurAlloc = &pOverlay->pResource->aAllocations[i];
        rc = vboxVhwaHlpDestroySurface(pOverlay->pDevExt, pCurAlloc, pOverlay->VidPnSourceId);
        AssertRC(rc);
    }

    if (RT_SUCCESS(rc))
    {
        int tmpRc = vboxVhwaHlpCheckTerm(pOverlay->pDevExt, pOverlay->VidPnSourceId);
        AssertRC(tmpRc);
    }

    return rc;
}


int vboxVhwaHlpOverlayCreate(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId, DXGK_OVERLAYINFO *pOverlayInfo,
        /* OUT */ PVBOXWDDM_OVERLAY pOverlay)
{
    int rc = vboxVhwaHlpCheckInit(pDevExt, VidPnSourceId);
    AssertRC(rc);
    if (RT_SUCCESS(rc))
    {
        PVBOXWDDM_ALLOCATION pAlloc = (PVBOXWDDM_ALLOCATION)pOverlayInfo->hAllocation;
        PVBOXWDDM_RESOURCE pRc = pAlloc->pResource;
        Assert(pRc);
        for (uint32_t i = 0; i < pRc->cAllocations; ++i)
        {
            PVBOXWDDM_ALLOCATION pCurAlloc = &pRc->aAllocations[i];
            rc = vboxVhwaHlpCreateSurface(pDevExt, pCurAlloc,
                        0, pRc->cAllocations - 1, VBOXVHWA_SCAPS_OVERLAY | VBOXVHWA_SCAPS_VIDEOMEMORY | VBOXVHWA_SCAPS_LOCALVIDMEM | VBOXVHWA_SCAPS_COMPLEX,
                        VidPnSourceId);
            AssertRC(rc);
            if (!RT_SUCCESS(rc))
            {
                int tmpRc;
                for (uint32_t j = 0; j < i; ++j)
                {
                    PVBOXWDDM_ALLOCATION pDestroyAlloc = &pRc->aAllocations[j];
                    tmpRc = vboxVhwaHlpDestroySurface(pDevExt, pDestroyAlloc, VidPnSourceId);
                    AssertRC(tmpRc);
                }
                break;
            }
        }

        if (RT_SUCCESS(rc))
        {
            pOverlay->pDevExt = pDevExt;
            pOverlay->pResource = pRc;
            pOverlay->VidPnSourceId = VidPnSourceId;

            vboxVhwaHlpOverlayListAdd(pDevExt, pOverlay);
#ifdef VBOXWDDM_RENDER_FROM_SHADOW
            RECT DstRect;
            vboxVhwaHlpOverlayDstRectGet(pDevExt, pOverlay, &DstRect);
            NTSTATUS Status = vboxVdmaHlpUpdatePrimary(pDevExt, VidPnSourceId, &DstRect);
            Assert(Status == STATUS_SUCCESS);
            /* ignore primary update failure */
            Status = STATUS_SUCCESS;
#endif
            rc = vboxVhwaHlpOverlayUpdate(pOverlay, pOverlayInfo, &DstRect);
            if (!RT_SUCCESS(rc))
            {
                int tmpRc = vboxVhwaHlpOverlayDestroy(pOverlay);
                AssertRC(tmpRc);
            }
        }

        if (RT_FAILURE(rc))
        {
            int tmpRc = vboxVhwaHlpCheckTerm(pDevExt, VidPnSourceId);
            AssertRC(tmpRc);
            AssertRC(rc);
        }
    }

    return rc;
}

BOOLEAN vboxVhwaHlpOverlayListIsEmpty(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[VidPnSourceId];
    return !ASMAtomicReadU32(&pSource->cOverlays);
}

#define VBOXWDDM_OVERLAY_FROM_ENTRY(_pEntry) ((PVBOXWDDM_OVERLAY)(((uint8_t*)(_pEntry)) - RT_OFFSETOF(VBOXWDDM_OVERLAY, ListEntry)))

void vboxVhwaHlpOverlayDstRectUnion(PDEVICE_EXTENSION pDevExt, D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId, RECT *pRect)
{
    if (vboxVhwaHlpOverlayListIsEmpty(pDevExt, VidPnSourceId))
    {
        memset(pRect, 0, sizeof (*pRect));
        return;
    }

    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[VidPnSourceId];
    KIRQL OldIrql;
    KeAcquireSpinLock(&pSource->OverlayListLock, &OldIrql);
    if (pSource->cOverlays)
    {
        PVBOXWDDM_OVERLAY pOverlay = VBOXWDDM_OVERLAY_FROM_ENTRY(pSource->OverlayList.Flink);
        *pRect = pOverlay->DstRect;
        while (pOverlay->ListEntry.Flink != &pSource->OverlayList)
        {
            pOverlay = VBOXWDDM_OVERLAY_FROM_ENTRY(pOverlay->ListEntry.Flink);
            vboxWddmRectUnite(pRect, &pOverlay->DstRect);
        }
    }
    KeReleaseSpinLock(&pSource->OverlayListLock, OldIrql);
}

void vboxVhwaHlpOverlayDstRectGet(PDEVICE_EXTENSION pDevExt, PVBOXWDDM_OVERLAY pOverlay, RECT *pRect)
{
    PVBOXWDDM_SOURCE pSource = &pDevExt->aSources[pOverlay->VidPnSourceId];
    KIRQL OldIrql;
    KeAcquireSpinLock(&pSource->OverlayListLock, &OldIrql);
    *pRect = pOverlay->DstRect;
    KeReleaseSpinLock(&pSource->OverlayListLock, OldIrql);
}
