/* $Id$ */
/** @file
 * IPRT - Linux sysfs access.
 */

/*
 * Copyright (C) 2006-2016 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP RTLOGGROUP_SYSTEM
#include <iprt/assert.h>
#include <iprt/dir.h>
#include <iprt/err.h>
#include <iprt/file.h>
#include <iprt/fs.h>
#include <iprt/param.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/symlink.h>

#include <iprt/linux/sysfs.h>

#include <unistd.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <errno.h>



/**
 * Constructs the path of a sysfs file from the format parameters passed,
 * prepending a prefix if the path is relative.
 *
 * @returns IPRT status code.
 * @param   pszPrefix  The prefix to prepend if the path is relative.  Must end
 *                     in '/'.
 * @param   pszBuf     Where to write the path.  Must be at least
 *                     sizeof(@a pszPrefix) characters long
 * @param   cchBuf     The size of the buffer pointed to by @a pszBuf.
 * @param   pszFormat  The name format, either absolute or relative to the
 *                     prefix specified by @a pszPrefix.
 * @param   va         The format args.
 */
static int rtLinuxConstructPathV(char *pszBuf, size_t cchBuf,
                                     const char *pszPrefix,
                                     const char *pszFormat, va_list va)
{
    size_t cchPrefix = strlen(pszPrefix);
    AssertReturn(pszPrefix[cchPrefix - 1] == '/', VERR_INVALID_PARAMETER);
    AssertReturn(cchBuf > cchPrefix + 1, VERR_INVALID_PARAMETER);

    /** @todo While RTStrPrintfV prevents overflows, it doesn't make it easy to
     *        check for truncations. RTPath should provide some formatters and
     *        joiners which can take over this rather common task that is
     *        performed here. */
    size_t cch = RTStrPrintfV(pszBuf, cchBuf, pszFormat, va);
    if (*pszBuf != '/')
    {
        AssertReturn(cchBuf >= cch + cchPrefix + 1, VERR_BUFFER_OVERFLOW);
        memmove(pszBuf + cchPrefix, pszBuf, cch + 1);
        memcpy(pszBuf, pszPrefix, cchPrefix);
        cch += cchPrefix;
    }
    return VINF_SUCCESS;
}


/**
 * Constructs the path of a sysfs file from the format parameters passed,
 * prepending a prefix if the path is relative.
 *
 * @returns IPRT status code.
 * @param   pszPrefix  The prefix to prepend if the path is relative.  Must end
 *                     in '/'.
 * @param   pszBuf     Where to write the path.  Must be at least
 *                     sizeof(@a pszPrefix) characters long
 * @param   cchBuf     The size of the buffer pointed to by @a pszBuf.
 * @param   pszFormat  The name format, either absolute or relative to "/sys/".
 * @param   ...        The format args.
 */
DECLINLINE(int) rtLinuxConstructPath(char *pszBuf, size_t cchBuf,
                                     const char *pszPrefix,
                                     const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = rtLinuxConstructPathV(pszBuf, cchBuf, pszPrefix, pszFormat, va);
    va_end(va);
    return rc;
}


/**
 * Constructs the path of a sysfs file from the format parameters passed,
 * prepending "/sys/" if the path is relative.
 *
 * @returns IPRT status code.
 * @param   pszBuf     Where to write the path.  Must be at least
 *                     sizeof("/sys/") characters long
 * @param   cchBuf     The size of the buffer pointed to by @a pszBuf.
 * @param   pszFormat  The name format, either absolute or relative to "/sys/".
 * @param   va         The format args.
 */
DECLINLINE(int) rtLinuxSysFsConstructPath(char *pszBuf, size_t cchBuf, const char *pszFormat, va_list va)
{
    return rtLinuxConstructPathV(pszBuf, cchBuf, "/sys/", pszFormat, va);
}


RTDECL(int) RTLinuxSysFsExistsExV(const char *pszFormat, va_list va)
{
    int iSavedErrno = errno;

    /*
     * Construct the filename and call stat.
     */
    char szFilename[RTPATH_MAX];
    int rc = rtLinuxSysFsConstructPath(szFilename, sizeof(szFilename), pszFormat, va);
    if (RT_SUCCESS(rc))
    {
        struct stat st;
        int rcStat = stat(szFilename, &st);
        if (rcStat != 0)
            rc = RTErrConvertFromErrno(errno);
    }

    errno = iSavedErrno;
    return rc;
}


RTDECL(bool) RTLinuxSysFsExistsV(const char *pszFormat, va_list va)
{
    return RT_SUCCESS(RTLinuxSysFsExistsExV(pszFormat, va));
}


RTDECL(int) RTLinuxSysFsExistsEx(const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsExistsExV(pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(bool) RTLinuxSysFsExists(const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    bool fRet = RTLinuxSysFsExistsV(pszFormat, va);
    va_end(va);
    return fRet;
}


RTDECL(int) RTLinuxSysFsOpenV(PRTFILE phFile, const char *pszFormat, va_list va)
{
    /*
     * Construct the filename and call open.
     */
    char szFilename[RTPATH_MAX];
    int rc = rtLinuxSysFsConstructPath(szFilename, sizeof(szFilename), pszFormat, va);
    if (RT_SUCCESS(rc))
        rc = RTFileOpen(phFile, szFilename, RTFILE_O_OPEN | RTFILE_O_READ | RTFILE_O_DENY_NONE);
    return rc;
}


RTDECL(int) RTLinuxSysFsOpenExV(PRTFILE phFile, uint64_t fOpen, const char *pszFormat, va_list va)
{
    /*
     * Construct the filename and call open.
     */
    char szFilename[RTPATH_MAX];
    int rc = rtLinuxSysFsConstructPath(szFilename, sizeof(szFilename), pszFormat, va);
    if (RT_SUCCESS(rc))
        rc = RTFileOpen(phFile, szFilename, fOpen);
    return rc;
}


RTDECL(int) RTLinuxSysFsOpen(PRTFILE phFile, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsOpenV(phFile, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsOpenEx(PRTFILE phFile, uint64_t fOpen, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsOpenExV(phFile, fOpen, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsReadStr(RTFILE hFile, char *pszBuf, size_t cchBuf, size_t *pcchRead)
{
    Assert(cchBuf > 1);
    size_t cchRead = 0;
    int rc = RTFileRead(hFile, pszBuf, cchBuf - 1, &cchRead);
    pszBuf[RT_SUCCESS(rc) ? cchRead : 0] = '\0';
    if (   RT_SUCCESS(rc)
        && pcchRead)
        *pcchRead = cchRead;
    if (RT_SUCCESS(rc))
    {
        /* Check for EOF */
        uint64_t offCur = 0;
        uint64_t offEnd = 0;
        rc = RTFileSeek(hFile, 0, RTFILE_SEEK_CURRENT, &offCur);
        if (RT_SUCCESS(rc))
            rc = RTFileSeek(hFile, 0, RTFILE_SEEK_END, &offEnd);
        if (   RT_SUCCESS(rc)
            && offEnd > offCur)
            rc = VERR_BUFFER_OVERFLOW;
    }
    return rc;
}


RTDECL(int) RTLinuxSysFsWriteStr(RTFILE hFile, const char *pszBuf, size_t cchBuf, size_t *pcchWritten)
{
    if (!cchBuf)
        cchBuf = strlen(pszBuf);
    return RTFileWrite(hFile, pszBuf, cchBuf, pcchWritten);
}


RTDECL(int) RTLinuxSysFsReadFile(RTFILE hFile, void *pvBuf, size_t cbBuf, size_t *pcbRead)
{
    int    rc;
    size_t cbRead = 0;

    rc = RTFileRead(hFile, pvBuf, cbBuf, &cbRead);
    if (RT_SUCCESS(rc))
    {
        if (pcbRead)
            *pcbRead = cbRead;
        if (cbRead < cbBuf)
            rc = VINF_SUCCESS;
        else
        {
            /* Check for EOF */
            uint64_t offCur = 0;
            uint64_t offEnd = 0;
            rc = RTFileSeek(hFile, 0, RTFILE_SEEK_CURRENT, &offCur);
            if (RT_SUCCESS(rc))
                rc = RTFileSeek(hFile, 0, RTFILE_SEEK_END, &offEnd);
            if (   RT_SUCCESS(rc)
                && offEnd > offCur)
                rc = VERR_BUFFER_OVERFLOW;
        }
    }

    return rc;
}


RTDECL(int) RTLinuxSysFsWriteFile(RTFILE hFile, void *pvBuf, size_t cbBuf, size_t *pcbWritten)
{
    return RTFileWrite(hFile, pvBuf, cbBuf, pcbWritten);
}


RTDECL(int) RTLinuxSysFsReadIntFileV(unsigned uBase, int64_t *pi64, const char *pszFormat, va_list va)
{
    RTFILE hFile;

    AssertPtrReturn(pi64, VERR_INVALID_POINTER);

    int rc = RTLinuxSysFsOpenV(&hFile, pszFormat, va);
    if (RT_SUCCESS(rc))
    {
        char szNum[128];
        size_t cchNum;
        rc = RTLinuxSysFsReadStr(hFile, szNum, sizeof(szNum), &cchNum);
        if (RT_SUCCESS(rc))
        {
            if (cchNum > 0)
            {
                int64_t i64Ret = -1;
                rc = RTStrToInt64Ex(szNum, NULL, uBase, &i64Ret);
                if (RT_SUCCESS(rc))
                    *pi64 = i64Ret;
            }
            else
                rc = VERR_INVALID_PARAMETER;
        }

        RTFileClose(hFile);
    }

    return rc;
}


RTDECL(int) RTLinuxSysFsReadIntFile(unsigned uBase, int64_t *pi64, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsReadIntFileV(uBase, pi64, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsWriteU8FileV(unsigned uBase, uint8_t u8, const char *pszFormat, va_list va)
{
    return RTLinuxSysFsWriteU64FileV(uBase, u8, pszFormat, va);
}


RTDECL(int) RTLinuxSysFsWriteU8File(unsigned uBase, uint8_t u8, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsWriteU64FileV(uBase, u8, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsWriteU16FileV(unsigned uBase, uint16_t u16, const char *pszFormat, va_list va)
{
    return RTLinuxSysFsWriteU64FileV(uBase, u16, pszFormat, va);
}


RTDECL(int) RTLinuxSysFsWriteU16File(unsigned uBase, uint16_t u16, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsWriteU64FileV(uBase, u16, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsWriteU32FileV(unsigned uBase, uint32_t u32, const char *pszFormat, va_list va)
{
    return RTLinuxSysFsWriteU64FileV(uBase, u32, pszFormat, va);
}


RTDECL(int) RTLinuxSysFsWriteU32File(unsigned uBase, uint32_t u32, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsWriteU64FileV(uBase, u32, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsWriteU64FileV(unsigned uBase, uint64_t u64, const char *pszFormat, va_list va)
{
    RTFILE hFile;

    const char *pszFmt = NULL;
    switch (uBase)
    {
        case 8:
            pszFmt = "%#llo";
            break;
        case 10:
            pszFmt = "%llu";
            break;
        case 16:
            pszFmt = "%#llx";
            break;
        default:
            return VERR_INVALID_PARAMETER;
    }

    int rc = RTLinuxSysFsOpenExV(&hFile, RTFILE_O_OPEN | RTFILE_O_WRITE | RTFILE_O_DENY_NONE, pszFormat, va);
    if (RT_SUCCESS(rc))
    {
        char szNum[128];
        size_t cchNum = RTStrPrintf(szNum, sizeof(szNum), pszFmt, u64);
        if (cchNum > 0)
        {
            size_t cbWritten = 0;
            rc = RTLinuxSysFsWriteStr(hFile, &szNum[0], cchNum, &cbWritten);
            if (   RT_SUCCESS(rc)
                && cbWritten != cchNum)
                rc = VERR_BUFFER_OVERFLOW;
        }
        else
            rc = VERR_INVALID_PARAMETER;

        RTFileClose(hFile);
    }

    return rc;
}


RTDECL(int) RTLinuxSysFsWriteU64File(unsigned uBase, uint32_t u64, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsWriteU64FileV(uBase, u64, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsReadDevNumFileV(dev_t *pDevNum, const char *pszFormat, va_list va)
{
    RTFILE hFile;

    AssertPtrReturn(pDevNum, VERR_INVALID_POINTER);

    int rc = RTLinuxSysFsOpenV(&hFile, pszFormat, va);
    if (RT_SUCCESS(rc))
    {
        size_t cchNum = 0;
        char szNum[128];
        rc = RTLinuxSysFsReadStr(hFile, szNum, sizeof(szNum), &cchNum);
        if (RT_SUCCESS(rc))
        {
            if (cchNum > 0)
            {
                uint32_t u32Maj = 0;
                uint32_t u32Min = 0;
                char *pszNext = NULL;
                rc = RTStrToUInt32Ex(szNum, &pszNext, 10, &u32Maj);
                if (RT_FAILURE(rc) || (rc != VWRN_TRAILING_CHARS) || (*pszNext != ':'))
                    rc = VERR_INVALID_PARAMETER;
                else
                {
                    rc = RTStrToUInt32Ex(pszNext + 1, NULL, 10, &u32Min);
                    if (   rc != VINF_SUCCESS
                        && rc != VWRN_TRAILING_CHARS
                        && rc != VWRN_TRAILING_SPACES)
                        rc = VERR_INVALID_PARAMETER;
                    else
                        *pDevNum = makedev(u32Maj, u32Min);
                }
            }
            else
                rc = VERR_INVALID_PARAMETER;
        }

        RTFileClose(hFile);
    }

    return rc;
}


RTDECL(int) RTLinuxSysFsReadDevNumFile(dev_t *pDevNum, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsReadDevNumFileV(pDevNum, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsReadStrFileV(char *pszBuf, size_t cchBuf, size_t *pcchRead, const char *pszFormat, va_list va)
{
    RTFILE hFile;

    AssertPtrReturn(pszBuf, VERR_INVALID_POINTER);

    int rc = RTLinuxSysFsOpenV(&hFile, pszFormat, va);
    if (RT_SUCCESS(rc))
    {
        size_t cchRead = 0;
        rc = RTLinuxSysFsReadStr(hFile, pszBuf, cchBuf, &cchRead);
        RTFileClose(hFile);
        if (   RT_SUCCESS(rc)
            && cchRead > 0)
        {
            char *pchNewLine = (char *)memchr(pszBuf, '\n', cchRead);
            if (pchNewLine)
                *pchNewLine = '\0';
        }

        if (pcchRead)
            *pcchRead = cchRead;
    }
    return rc;
}


RTDECL(int) RTLinuxSysFsReadStrFile(char *pszBuf, size_t cchBuf, size_t *pcchRead, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsReadStrFileV(pszBuf, cchBuf, pcchRead, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxSysFsWriteStrFileV(const char *pszBuf, size_t cchBuf, size_t *pcchWritten, const char *pszFormat, va_list va)
{
    RTFILE hFile;

    AssertPtrReturn(pszBuf, VERR_INVALID_POINTER);

    int rc = RTLinuxSysFsOpenExV(&hFile, RTFILE_O_OPEN | RTFILE_O_WRITE | RTFILE_O_DENY_NONE, pszFormat, va);
    if (RT_SUCCESS(rc))
    {
        rc = RTLinuxSysFsWriteStr(hFile, pszBuf, cchBuf, pcchWritten);
        RTFileClose(hFile);
    }
    return rc;
}


RTDECL(int) RTLinuxSysFsWriteStrFile(const char *pszBuf, size_t cchBuf, size_t *pcchWritten, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsWriteStrFileV(pszBuf, cchBuf, pcchWritten, pszFormat, va);
    va_end(va);
    return rc;
}

RTDECL(int) RTLinuxSysFsGetLinkDestV(char *pszBuf, size_t cchBuf, size_t *pchBuf, const char *pszFormat, va_list va)
{
    AssertReturn(cchBuf >= 2, VERR_INVALID_PARAMETER);

    /*
     * Construct the filename and read the link.
     */
    char szFilename[RTPATH_MAX];
    int rc = rtLinuxSysFsConstructPath(szFilename, sizeof(szFilename), pszFormat, va);
    if (RT_SUCCESS(rc))
    {
        char szLink[RTPATH_MAX];
        rc = RTSymlinkRead(szFilename, szLink, sizeof(szLink), 0);
        if (RT_SUCCESS(rc))
        {
            /*
             * Extract the file name component and copy it into the return buffer.
             */
            size_t cchName;
            const char *pszName = RTPathFilename(szLink);
            if (pszName)
            {
                cchName = strlen(pszName);
                if (cchName < cchBuf)
                    memcpy(pszBuf, pszName, cchName + 1);
                else
                    rc = VERR_BUFFER_OVERFLOW;
            }
            else
            {
                *pszBuf = '\0';
                cchName = 0;
            }

            if (pchBuf)
                *pchBuf = cchName;
        }
    }

    return rc;
}


RTDECL(int) RTLinuxSysFsGetLinkDest(char *pszBuf, size_t cchBuf, size_t *pchBuf, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    int rc = RTLinuxSysFsGetLinkDestV(pszBuf, cchBuf, pchBuf, pszFormat, va);
    va_end(va);
    return rc;
}


RTDECL(int) RTLinuxCheckDevicePathV(dev_t DevNum, RTFMODE fMode, char *pszBuf,
                                    size_t cchBuf, const char *pszPattern,
                                    va_list va)
{
    AssertReturn(cchBuf >= 2, VERR_INVALID_PARAMETER);
    AssertReturn(   fMode == RTFS_TYPE_DEV_CHAR
                 || fMode == RTFS_TYPE_DEV_BLOCK,
                 VERR_INVALID_PARAMETER);
    AssertPtrReturn(pszPattern, VERR_INVALID_PARAMETER);

    /*
     * Construct the filename and read the link.
     */
    char szFilename[RTPATH_MAX];
    int rc = rtLinuxConstructPathV(szFilename, sizeof(szFilename), "/dev/",
                                   pszPattern, va);
    if (RT_SUCCESS(rc))
    {
        RTFSOBJINFO Info;
        rc = RTPathQueryInfo(szFilename, &Info, RTFSOBJATTRADD_UNIX);
        if (   rc == VERR_PATH_NOT_FOUND
            || (   RT_SUCCESS(rc)
                && (   Info.Attr.u.Unix.Device != DevNum
                    || (Info.Attr.fMode & RTFS_TYPE_MASK) != fMode)))
            rc = VERR_FILE_NOT_FOUND;

        if (RT_SUCCESS(rc))
        {
            size_t cchPath = strlen(szFilename);
            if (cchPath < cchBuf)
                memcpy(pszBuf, szFilename, cchPath + 1);
            else
                rc = VERR_BUFFER_OVERFLOW;
        }
    }

    return rc;
}


RTDECL(int) RTLinuxCheckDevicePath(dev_t DevNum, RTFMODE fMode, char *pszBuf,
                                   size_t cchBuf, const char *pszPattern,
                                   ...)
{
    va_list va;
    va_start(va, pszPattern);
    int rc = RTLinuxCheckDevicePathV(DevNum, fMode, pszBuf, cchBuf,
                                     pszPattern, va);
    va_end(va);
    return rc;
}

