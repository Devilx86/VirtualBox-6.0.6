#!/usr/bin/perl
# $Id$
## @file
# This little perl program attempts to connect to a running VirtualBox
# webservice and calls various methods on it. Please refer to the SDK
# programming reference (SDKRef.pdf) for how to use this sample.
#
# Note! The following license applies to this file only
#

#
# Copyright (C) 2008-2017 Oracle Corporation
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation
# files (the "Software"), to deal in the Software without
# restriction, including without limitation the rights to use,
# copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following
# conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
# OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

use strict;
use SOAP::Lite;
use vboxService;    # generated by stubmaker, see SDKRef.pdf
use Data::Dumper;

my $cmd = 'clienttest';
my $optMode;
my $vmname;
my $disk;

while (my $this = shift(@ARGV))
{
    if (($this =~ /^-h/) || ($this =~ /^--help/))
    {
        print "$cmd: test the VirtualBox web service.\n".
              "Usage:\n".
              "    $cmd <mode>\n".
              "with <mode> being one of 'version', 'list', 'start'; default is 'list'.\n".
              "    $cmd version: print version of VirtualBox web service.\n".
              "    $cmd list: list installed virtual machines.\n".
              "    $cmd startvm <vm>: start the virtual machine named <vm>.\n".
              "    $cmd acpipowerbutton <vm>: shutdown of the irtual machine named <vm>.\n";
              "    $cmd openhd <disk>: open disk image <disk>.\n";
        exit 0;
    }
    elsif (    ($this eq 'version')
            || ($this eq 'list')
          )
    {
        $optMode = $this;
    }
    elsif (    ($this eq 'startvm')
            || ($this eq 'acpipowerbutton')
          )
    {
        $optMode = $this;

        if (!($vmname = shift(@ARGV)))
        {
            die "[$cmd] Missing parameter: You must specify the name of the VM to start.\nStopped";
        }
    }
    elsif ($this eq 'openhd')
    {
        $optMode = $this;

        if (!($disk = shift(@ARGV)))
        {
            die "[$cmd] Missing parameter: You must specify the name of the disk to open.\nStopped";
        }
    }
    else
    {
        die "[$cmd] Unknown option \"$this\"; stopped";
    }
}

$optMode = "list"
    if (!$optMode);

# SOAP::Lite hacking to make it serialize the enum types we use correctly.
# In the long run, this needs to be done either by stubmaker.pl or something
# else, because the WSDL clearly says they're restricted strings. Quite silly
# that the default behavior is to ignore the parameter and just let the server
# use the default value for the type.

sub SOAP::Serializer::as_LockType
{
    my ($self, $value, $name, $type, $attr) = @_;
    die "String value expected instead of @{[ref $value]} reference\n"
        if ref $value;
    return [
        $name,
        {'xsi:type' => 'vbox:LockType', %$attr},
        SOAP::Utils::encode_data($value)
    ];
}

sub SOAP::Serializer::as_DeviceType
{
    my ($self, $value, $name, $type, $attr) = @_;
    die "String value expected instead of @{[ref $value]} reference\n"
        if ref $value;
    return [
        $name,
        {'xsi:type' => 'vbox:DeviceType', %$attr},
        SOAP::Utils::encode_data($value)
    ];
}

sub SOAP::Serializer::as_AccessMode
{
    my ($self, $value, $name, $type, $attr) = @_;
    die "String value expected instead of @{[ref $value]} reference\n"
        if ref $value;
    return [
        $name,
        {'xsi:type' => 'vbox:AccessMode', %$attr},
        SOAP::Utils::encode_data($value)
    ];
}

## @todo needs much more error handling, e.g. openhd never complains

my $vbox = vboxService->IWebsessionManager_logon("test", "test");

if (!$vbox)
{
    die "[$cmd] Logon to session manager with user \"test\" and password \"test\" failed.\nStopped";
}

if ($optMode eq "version")
{
    my $v = vboxService->IVirtualBox_getVersion($vbox);
    print "[$cmd] Version number of running VirtualBox web service: $v\n";
}
elsif ($optMode eq "list")
{
    print "[$cmd] Listing machines:\n";
    my @result = vboxService->IVirtualBox_getMachines($vbox);
    foreach my $idMachine (@result)
    {
        my $if = vboxService->IManagedObjectRef_getInterfaceName($idMachine);
        my $name = vboxService->IMachine_getName($idMachine);

        print "machine $if $idMachine: $name\n";
    }
}
elsif ($optMode eq "startvm")
{
    my $machine = vboxService->IVirtualBox_findMachine($vbox, $vmname);

    die "[$cmd] Cannot find VM \"$vmname\"; stopped"
        if (!$machine);

    my $session = vboxService->IWebsessionManager_getSessionObject($vbox);
    die "[$cmd] Cannot get session object; stopped"
        if (!$session);

    my $uuid = vboxService->IMachine_getId($machine);
    die "[$cmd] Cannot get uuid for machine; stopped"
        if (!$uuid);

    print "[$cmd] UUID: $uuid\n";

    my $progress = vboxService->IMachine_launchVMProcess($machine,
                                                         $session,
                                                         "headless",
                                                         "");
    die "[$cmd] Cannot launch VM; stopped"
        if (!$progress);

    print("[$cmd] Waiting for the VM to start...\n");
    vboxService->IProgress_waitForCompletion($progress, -1);

    my $fCompleted;
    $fCompleted = vboxService->IProgress_getCompleted($progress);
    print("[$cmd] Completed: $fCompleted\n");

    my $resultCode;
    $resultCode = vboxService->IProgress_getResultCode($progress);

    print("[$cmd] Result: $resultCode\n");

    vboxService->ISession_unlockMachine($session);

    vboxService->IWebsessionManager_logoff($vbox);
}
elsif ($optMode eq "acpipowerbutton")
{
    my $machine = vboxService->IVirtualBox_findMachine($vbox, $vmname);

    die "[$cmd] Cannot find VM \"$vmname\"; stopped"
        if (!$machine);

    my $session = vboxService->IWebsessionManager_getSessionObject($vbox);
    die "[$cmd] Cannot get session object; stopped"
        if (!$session);

    vboxService->IMachine_lockMachine($machine, $session, 'Shared');

    my $console = vboxService->ISession_getConsole($session);

    vboxService->IConsole_powerButton($console);

    vboxService->ISession_unlockMachine($session);

    vboxService->IWebsessionManager_logoff($vbox);
}
elsif ($optMode eq "openhd")
{
    my $medium = vboxService->IVirtualBox_openMedium($vbox, $disk,
                                                     'HardDisk',
                                                     'ReadWrite',
                                                     0);
}
