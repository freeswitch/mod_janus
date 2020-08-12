# mod_janus

The mod_janus endpoint provides an interface to the Janus [audiobridge](https://janus.conf.meetecho.com/docs/audiobridge.html).

This allows legacy POTS to join the same room as the WebRTC users that are already supported by Janus.

The module will only support audio calls - video calls will be rejected.  The long polling HTTP interface is used in communication with Janus.  No provision is given to the Websocket interface.

## Table of Contents

* [Table of Contents](#table-of-contents)
* [License](#license)
* [Contributor(s)](#contributors)
* [Features](#features)
* [Build and install mod_janus](#build-and-install-mod_janus)
* [Configuration](#configuration)
* [Usage](#usage)
* [Command Line Interface (CLI)](#command-line-interface-cli)
* [Notes](#notes)
* [Troubleshooting](#troubleshooting)

## License

FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>

Version: MPL 1.1

The contents of this file are subject to the Mozilla Public License Version 1.1 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at <http://www.mozilla.org/MPL/>

Software distributed under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for the specific language governing rights and limitations under the License.

The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application

The Initial Developer of the Original Code is Anthony Minessale II <anthm@freeswitch.org>
Portions created by the Initial Developer are Copyright (C) the Initial Developer. All Rights Reserved.

## Contributor(s)

* Richard Screene (rscreene@yahoo.co.uk)


## Build and install mod_janus

Change to a directory where the FreeSWITCH sources will be compiled

```
cd /usr/src
```

Clone the FreeSWITCH repository into the above directory

```
git clone https://github.com/signalwire/freeswitch.git
```

Perform an initial bootstrap of FreeSWITCH so that a `modules.conf` file is created

```
./bootstrap.sh
```

Add the mod_janus to `modules.conf` so that an out-of-source build will be performed

```
mod_janus|https://github.com/freeswitch/mod_janus.git -b master
```

Configure, build and install FreeSWITCH

```
./configure
make
make install
```

The following commands will build and install *only* mod_janus

```
make mod_janus
make mod_janus-install
```

To run mod_janus within FreeSWITCH, perform the following two steps
1. Add mod_janus to freeswitch/conf/autoload/modules.conf.xml
2. Add autoload_conf/janus.conf.xml to freeswitch/conf/autoload_configs

## Configuration

The configuration file consists of two sections:
1. A settings section that currently only contains the debug flag, and
2. A list of Janus servers to connect to.  Multiple servers may be defined the module can route calls to any of them.

Each server contains the following fields:
* name - is the internal name given to the server that must be specified in the dial string.
* url - is the address of the server
* secret - is the API secret required by Janus (if it has been enabled on the Janus end)
* auth-token - is the token string string added to the Janus poll request
* enabled - defines if the server should be brought into service when the module starts.  This state may be modified by the console API.  The default is false.
* rtp-ip - [see mod_sofia](https://freeswitch.org/confluence/display/FREESWITCH/mod_sofia)
* ext-rtp-ip - [see mod_sofia](https://freeswitch.org/confluence/display/FREESWITCH/mod_sofia)
* apply-candidate-acl - [see mod_sofia](https://freeswitch.org/confluence/display/FREESWITCH/mod_sofia) (default is none)
* local-network-acl - [see mod_sofia](https://freeswitch.org/confluence/display/FREESWITCH/mod_sofia) (default is "localnet.auto")
* codec-string - the list of codecs that should be offered to Janus.  Should always be Opus which is the default.

## Usage

If called with the following dialstring (`{janus-use-existing-room=true}janus/demo/MyName@1234`) this configuration file should allow you to test using the Janus [audiobridge demo](https://janus.conf.meetecho.com/audiobridgetest.html).

```
<configuration name="janus.conf" description="Janus Endpoint">
  <settings>
    <param name="debug" value="false"/>
  </settings>

  <server name="demo">
    <param name="url" value="https://janus.conf.meetecho.com/janus"/>
    <!-- <param name="secret" value="the-secret"/> -->
    <!-- <param name="auth-token" value="the-auth-token"/> -->
    <param name="enabled" value="true"/>
    <param name="rtp-ip" value="$${bind_server_ip}"/>
    <!-- <param name="apply-candidate-acl" value="localnet.auto"/> -->
    <!-- <param name="local-network-acl" value="localnet.auto"/> -->
    <param name="ext-rtp-ip" value="auto-nat"/>
    <param name="codec-string" value="opus"/>
  </server>
</configuration>
```

The following channel variables are defined:
* janus-use-existing-room - By default the module will create the room, if this flag is set then the caller is joined to an existing room.
* janus-room-description - This is a textual description of the room specified in the *create* request to Janus (only applicable if janus-use-existing-room is false)
* janus-room-record - The value is specified in the *create* request to Janus and indicates that the room mix should be recorded (only applicable if janus-use-existing-room is false).  The default value is not to record.
* janus-room-record-file - This value specifies the file name to which the recording should be written.  It is passed in the *create* request to Janus (only applicable if janus-use-existing-room is false and  janus-room-record is true).  If omitted the default filename will be used.
* janus-room-pin - PIN that is used to validate a user entering the room.  This value is used to set the PIN for a created room.
* janus-user-token - The token for the user that should be passed in the *join* request.  NB. no method is provided to set the allowed tokens in the *create* room request.
* janus-user-record - Janus should generate a file containing the audio from the user only.  It is specified in the *configure* request.  The default value is not to record.
* janus-user-record-file - This specifies the base of the filename used when recording the user audio stream.  If omitted the default filename will be used.
* janus-start-muted - Included in the *confifigure* request to indicate that the user should enter the room muted (no mechanism exists in the module to modify the mute status later).  The default value is that the user should not be muted.

The dial string is composed of the following parts:
```
/janus/<server>/<display name>@<room>
```

## Command Line Interface (CLI)

The following commands are available on the console API:
* janus debug [true|false]  - enables debug on/off
* janus list - lists all the servers with the following values: name, enabled, total calls, calls in progress, start timestamp (usec) and the internal server id
* janus server <name> [enable|disable] - set the server active or inactive.  NB because we have to wait for the long poll to complete this may take around 30 seconds.
* janus status - totalled for all servers the following are reported: total calls, calls in progress, start timestamp (usec)

## Notes

TODO: Use websocket rather than long-polling for connection to Janus
TODO: I am not convinced that the shutdown is always successful

## Troubleshooting

For test purposes it is possible to use the Janus [audiobridge demo](https://janus.conf.meetecho.com/audiobridgetest.html) by adding something like this to the dialplan:
```
<condition field="destination_number" expression="^(\d{11,})$">
  <action application="answer" data=""/>
  <action application="log" data="INFO Joining Call to Conference"/>
  <action application="bridge" data="{janus-use-existing-room=true,janus-start-muted=true}janus/demo/Peter@1234"/>
</condition>
```
