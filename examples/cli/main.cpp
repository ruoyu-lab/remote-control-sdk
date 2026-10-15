// Command-line share/view client built only on the public C++ interface.
//
//   rcsdk_cli --generate-key key.bin
//   rcsdk_cli --generate-pin 6
//   rcsdk_cli --info
//   rcsdk_cli --list-windows
//   rcsdk_cli --thumbnail shot.bmp --window 0x1234 [--max-size 320,240]
//   rcsdk_cli --role share --address 192.168.1.10 --key-file key.bin [--allow-control] [--codec hevc,h264]
//   rcsdk_cli --role share --address 192.168.1.10 --key-file key.bin --preset low-latency
//   rcsdk_cli --role view  --address 192.168.1.10 --key-file key.bin --seconds 30 --report stats.json
//   rcsdk_cli --role share --pin 123456 [--pin-attempts 5]; rcsdk_cli --role view --address 192.168.1.10 --pin 123456
//   rcsdk_cli --role share --transport p2p --signal-relay 203.0.113.7:47910 --key-file key.bin
//   rcsdk_cli --role view --transport p2p --signal-relay 203.0.113.7:47910 --key-file key.bin --viewers 3
//   rcsdk_cli --role share ... --receive-dir ~/Downloads; rcsdk_cli --role view ... --send-file a.pdf --send-file photos
//   rcsdk_cli --role view --address 192.168.1.10 --key-file key.bin --record rec [--record-only]
//   rcsdk_cli --wake-info; rcsdk_cli --wake 00:1b:21:3a:4b:5c --wake-address 192.168.1.255
//   rcsdk_cli --role share ... --allow-control --power-actions restart,sleep; rcsdk_cli --role view ... --power-ask restart
#include <rcsdk/agent.hpp>
#include <rcsdk/clipboard.h>
#include <rcsdk/file_transfer.h>
#include <rcsdk/host_codec.hpp>
#include <rcsdk/power.h>
#include <rcsdk/rate_control.hpp>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/text_caret.h>
#include <rcsdk/thumbnail.h>
#include <rcsdk/tuning.hpp>

#include "../common/recorder.hpp"
#include "../common/relay_channel.hpp"
#include "../pointer/pointer_module.h"
#include "../common/signal_board.hpp"
#include "../../tools/visual_marker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {

using namespace std::chrono_literals;

std::atomic<bool> interrupted{false};

void waitFor(std::chrono::milliseconds duration) {
#ifdef __APPLE__
    // Keep the main run loop serviced for system frameworks that post to it.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(duration).count(), false);
#else
    std::this_thread::sleep_for(duration);
#endif
}

struct Arguments {
    std::map<std::string, std::string> values;            // The last of each...
    std::map<std::string, std::vector<std::string>> lists; // ...and all, for those that repeat.
    std::vector<std::string> flags;
    bool has(const std::string& name) const {
        return values.count(name) || std::find(flags.begin(), flags.end(), name) != flags.end();
    }
    std::string get(const std::string& name, const std::string& fallback = {}) const {
        const auto found = values.find(name);
        return found == values.end() ? fallback : found->second;
    }
    uint32_t number(const std::string& name, uint32_t fallback) const {
        return values.count(name) ? uint32_t(std::stoul(values.at(name))) : fallback;
    }
    std::vector<std::string> all(const std::string& name) const {
        const auto found = lists.find(name);
        return found == lists.end() ? std::vector<std::string>() : found->second;
    }
};

Arguments parse(int argc, char** argv) {
    static const std::vector<std::string> flagNames = {
        "--info",                "--list-windows",       "--allow-control",
        "--help",                "--exclude-own-sound",  "--no-playback",
        "--agent",               "--secure-desktop",     "--send-attention",
        "--restart-notice",      "--relay-datagram",     "--host-pattern",
        "--host-codec",          "--encoded-pattern",    "--text-caret",
        "--clipboard-concealed", "--clipboard-existing", "--pointer",
        "--record-only",         "--list-audio-devices", "--no-voice-processing",
        "--host-input",          "--pattern-marker",     "--clipboard-host",
        "--wake-info",           "--share-wake-info",    "--clipboard-files",
        "--standby"};
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (std::find(flagNames.begin(), flagNames.end(), name) != flagNames.end()) {
            arguments.flags.push_back(name);
        } else if (name.rfind("--", 0) == 0 && i + 1 < argc) {
            arguments.values[name] = argv[++i];
            arguments.lists[name].push_back(argv[i]);
        } else {
            throw std::invalid_argument("Unexpected argument: " + name);
        }
    }
    return arguments;
}

void usage() {
    std::cout << "Usage:\n"
                 "  rcsdk_cli --generate-key FILE\n"
                 "  rcsdk_cli --generate-device-key FILE   (prints its public key)\n"
                 "  rcsdk_cli --generate-pin DIGITS        (prints a PIN of 4..32 random digits)\n"
                 "  rcsdk_cli --info\n"
                 "  rcsdk_cli --list-windows\n"
                 "  rcsdk_cli --thumbnail FILE [--display ID | --region X,Y,W,H | --window ID | --application PID]\n"
                 "            [--max-size W,H]   a picture of what would be shared, as a .bmp (default: the main\n"
                 "            display, at most 320,320)\n"
                 "  rcsdk_cli --role share|view --address IP --key-file FILE [options]   (IP: numeric IPv4 or IPv6)\n"
                 "  rcsdk_cli --agent --agent-endpoint NAME --agent-key-file FILE [--seconds N]\n"
                 "  rcsdk_cli --wake-info               this machine's network interfaces that can wake it, and\n"
                 "                                      the power actions this process can carry out\n"
                 "  rcsdk_cli --wake MAC [--wake-address A] [--wake-port N] [--wake-password HEX] [--wake-count N]\n"
                 "            a Wake-on-LAN packet to MAC (aa:bb:cc:dd:ee:ff), N times a second apart (default 3),\n"
                 "            to A (default 255.255.255.255; better the subnet's broadcast address, which\n"
                 "            --wake-info and --share-wake-info print) at port N (default 9)\n"
                 "Options:\n"
                 "  --preset NAME       balanced (default), low-latency, low-bandwidth or quality: the settings no\n"
                 "                      option below changes (the defaults named below are balanced's)\n"
                 "  --port N            port (47990)\n"
                 "  --password TEXT     authenticate with a password instead of --key-file (10+ random characters)\n"
                 "  --pin DIGITS        authenticate with a short PIN instead of --key-file (see --generate-pin)\n"
                 "  --pin-attempts N    share: failed PIN handshakes allowed before PINs are refused (5)\n"
                 "  --secret none       no shared secret: device keys alone (needs --device-key and --trust)\n"
                 "  --device-key FILE   prove this device with its key (see --generate-device-key)\n"
                 "  --trust HEX[,HEX]   accept only peers proving one of these public keys\n"
                 "  --width/--height N  share: encoded size (1920x1080); view: the size to ask for\n"
                 "  --fps N --bitrate BPS   share: the stream's; view: what to ask for\n"
                 "  --display ID        share: the display (rcsdk_cli --info lists them)\n"
                 "  --region X,Y,W,H    share: only this rectangle of the display, in its pixels\n"
                 "  --window ID         share: only this window (0x... or decimal; rcsdk_cli --list-windows)\n"
                 "  --application PID   share: every window of this process on the display\n"
                 "  --streams ID[,ID]   share: more displays, each a stream of its own beside --display's\n"
                 "  --receive-streams N view: the streams to take, as bits (1: stream 0, 2: stream 1, ...); 0: all\n"
                 "  --host-pattern      share: moving bars and grain pushed as the host's own frames, not the screen\n"
                 "  --pattern-marker    share, with --host-pattern --allow-control --host-input: bars without grain\n"
                 "                      and rcsdk_response_probe's marker, which the viewer's Space advances: the\n"
                 "                      response from key to picture without a screen\n"
                 "  --host-codec        both ends: a codec of the hosts' own (host codec 16: pictures shrunk 16 times\n"
                 "                      each way), by the host's encoder and decoder; H.264 stays the alternative\n"
                 "  --encoded-pattern   share: push a host-codec-16 bitstream of its own (capture source ENCODED)\n"
                 "  --keyframe-interval MS  share: a keyframe at least this often (default: when needed)\n"
                 "  --rate-rules O,S,C  share: adaptive bitrate's queue thresholds in ms: overuse, severe, clear\n"
                 "                      (default 30,60,10)\n"
                 "  --rate-cap KBPS     share: decide each report as the rules do, but at most this (on_rate_report)\n"
                 "  --rate-trace FILE   share: write each report and decision as CSV\n"
                 "  --pacing US,MBPS    share: each picture leaves within US microseconds (0: at once), at most at\n"
                 "                      MBPS Mbit/s, while the path is clear (default 1000,800)\n"
                 "  --path-pacing PERCENT,US  share: beyond a picture's first US microseconds of the path's time,\n"
                 "                      at most PERCENT of the rate the viewer measures the path at (default\n"
                 "                      110,8000; 0,0: no such limit)\n"
                 "  --fec off|auto|fixed[,PERCENT]   share: parity for lost video on datagram paths (default\n"
                 "                      auto,50: none until the viewer reports loss)\n"
                 "  --codec LIST        codecs to offer, preferred first, of h264, hevc, vp8, vp9, av1 (default: h264)\n"
                 "  --formats LIST      picture formats to offer, preferred first, of 420, 420-10, 444, 444-10,\n"
                 "                      hdr10, hdr10-444 (default: 420); view: what this end takes\n"
                 "  --color M,R         share: SDR pictures' matrix bt709|bt601 and the range limited|full\n"
                 "                      (default bt709,limited)\n"
                 "  --request-format F  view: ask the sharer for one of --formats\n"
                 "  --frames gpu|cpu    view: take the decoded pictures (GPU surfaces or BGRA8 on the CPU) and\n"
                 "                      print the first one's layout and colour (default: neither)\n"
                 "  --save-frame FILE   view, with --frames cpu: the last picture, as a .bmp when the run ends\n"
                 "  --record PREFIX     view: write the packets as they arrive, without encoding again: the picture\n"
                 "                      as PREFIX.h264, .h265 (Annex B) or .obu (AV1) with each picture's time in\n"
                 "                      <file>.timestamps, the sound as PREFIX.ogg (Opus), .aac (ADTS) or .wav\n"
                 "                      (PCM); mkvmerge -o rec.mkv --timestamps 0:rec.h264.timestamps rec.h264\n"
                 "                      rec.ogg puts them together\n"
                 "  --record-only       view, with --record: do not decode or play anything (decode 0)\n"
                 "  --acceleration hardware|software|hardware-first   codec implementation (default: hardware;\n"
                 "                      hardware-first: software for what the hardware cannot encode or decode)\n"
                 "  --backend auto|mf|nvenc|nvdec|onevpl|vt|mediacodec|openh264|libvpx|aom|dav1d   codec\n"
                 "                      backend: Media Foundation, NVENC and NVDEC (Windows, NVIDIA), oneVPL\n"
                 "                      (Windows, Intel), VideoToolbox (macOS, iOS), MediaCodec (Android), or a\n"
                 "                      library built into the SDK (default: auto)\n"
                 "  --content auto|detail|motion   share: what the pictures are, for the libraries' encoders\n"
                 "  --software-threads N   threads of each software encoder and decoder of the libraries\n"
                 "  --encoder-speed fastest|balanced|quality   share: time per picture spent on quality\n"
                 "                      (default: fastest)\n"
                 "  --degradation frame-rate|size|balanced   share: what gives way when the bitrate does not\n"
                 "                      carry the stream (default: frame-rate)\n"
                 "  --input-boost-ms N  share: capture at twice the rate for N ms after remote input\n"
                 "  --allow-control     share: let the viewer control this machine\n"
                 "  --host-input        share, with --allow-control: the viewer's input comes to this program\n"
                 "                      (printed), not to the system (RCS_INPUT_HOST)\n"
                 "  --caps-lock input-source|caps-lock   share, with --allow-control: what the viewer's Caps Lock\n"
                 "                      does on a Mac: switch input sources as its own keyboard does (default),\n"
                 "                      or only toggle Caps Lock\n"
                 "  --admit-after N     share: decide on each viewer N seconds after it came, as a host that asks\n"
                 "                      its user would (it waits meanwhile); admitted unless --refuse-viewers\n"
                 "  --refuse-viewers REASON   share: refuse each viewer (at once, or after --admit-after), saying\n"
                 "                      REASON\n"
                 "  --keep-awake system|display[,run]   both ends: the power module keeps this machine (or also\n"
                 "                      its display) awake while the other end is connected, or with ,run\n"
                 "                      the whole run\n"
                 "  --power-actions LIST   share: viewers holding remote control may ask this machine to\n"
                 "                      restart, shut-down, sleep or sign-out (comma-separated, or all) - for\n"
                 "                      real: it happens --power-delay ms after the answer (default 3000)\n"
                 "  --share-wake-info   share: tell viewers this machine's interfaces that can wake it (MAC\n"
                 "                      addresses); the viewer prints them\n"
                 "  --power-ask ACTION[,force]   view: once the sharer said what it offers, ask its machine to\n"
                 "                      restart, shut-down, sleep or sign-out (force: applications are closed,\n"
                 "                      unsaved work lost) and print the answers\n"
                 "  --text-caret        both ends: the text caret module; the sharer tells where text goes on\n"
                 "                      its machine, the viewer asks and prints it\n"
                 "  --session-thumbnail FILE   share: the thumbnail module; once it runs where the session\n"
                 "                      captures (an agent: --agent-endpoint), one picture of the main display\n"
                 "                      taken there, as a .bmp\n"
                 "  --clipboard send|receive|both   this machine's clipboard goes to the other end, the other\n"
                 "                      end's comes here, or both (default: neither)\n"
                 "  --clipboard-concealed   with --clipboard: copies marked as passwords go too\n"
                 "  --clipboard-existing    with --clipboard: what this machine held before connecting goes too\n"
                 "  --clipboard-host    with --clipboard: the program's own clipboard (target HOST), not the\n"
                 "                      machine's: copies that arrive are printed, --clipboard-text sends one\n"
                 "  --clipboard-text TEXT   with --clipboard-host: TEXT goes as a copy once the other end takes it\n"
                 "  --clipboard-files   with --clipboard: files copied go too (FILES, both ends; file transfer\n"
                 "                      carries them as the other end pastes): the system's paste takes them,\n"
                 "                      or with --clipboard-host and --receive-dir they are fetched there\n"
                 "  --clipboard-file PATH   with --clipboard-host and --clipboard-files: PATH (a file or a\n"
                 "                      directory; repeat for more) goes as a copy, beside --clipboard-text\n"
                 "  --pointer           both ends: the example module com.example.pointer; the viewer points in\n"
                 "                      circles, the sharer prints where (every 20th)\n"
                 "  --send-file PATH    both ends: the file transfer module; PATH (a file, or a directory with all\n"
                 "                      it holds; repeat for more) goes as one transfer once the other end\n"
                 "                      receives files (a sharer: to each viewer that does)\n"
                 "  --receive-dir DIR   both ends: the file transfer module; the other end's files are accepted\n"
                 "                      into DIR (an existing directory)\n"
                 "  --file-conflict overwrite|rename|skip|fail   with --receive-dir: a name already there\n"
                 "                      (default: rename)\n"
                 "  --publish PATH      both ends: the file transfer module; PATH (repeat for more) is published\n"
                 "                      to the other end once it receives files (each viewer of a sharer): its\n"
                 "                      id is printed, nothing goes until the other end fetches it\n"
                 "  --publish-out FILE  with --publish: each id is written to FILE too\n"
                 "  --fetch ID|FILE     with --receive-dir: the other end's publication ID (32 hex digits, or\n"
                 "                      the first line of FILE once it holds them) is fetched into the\n"
                 "                      directory once the other end sends files\n"
                 "  --fetch-read ID     with --receive-dir: the same, read piece by piece (as a system paste\n"
                 "                      reads) and written below the directory by this program\n"
                 "  --secure-desktop    share, Windows: capture and input follow the sign-in screen, lock screen\n"
                 "                      and UAC prompts (needs a LocalSystem process; see rcsdk_unattended)\n"
                 "  --send-attention    view: send Ctrl+Alt+Del once control is granted\n"
                 "  --pause WHAT,AFTER,FOR   both ends: pause video, audio or both AFTER seconds after the run\n"
                 "                      started (a sharer: its first viewer came), and resume FOR seconds later\n"
                 "                      (rcs_session_pause)\n"
                 "  --type TEXT         view: type TEXT (letters, digits, space, . - _ @) once control is granted\n"
                 "                      as key presses\n"
                 "  --text TEXT         view: type TEXT (any Unicode, UTF-8) as text events once control is granted\n"
                 "  --restart-notice    stop with RCS_STOP_RESTART: the peer reconnects (a viewer) or listens again\n"
                 "  --agent-endpoint E  share: capture, encode and input run in an agent attached through E\n"
                 "                      (Windows: a pipe name; elsewhere: the absolute path of a socket file)\n"
                 "  --agent-key-file F  the key the session and its agent share (--generate-key)\n"
                 "  --agent-access privileged|any   share: who may open the endpoint (default: privileged)\n"
                 "  --agent             run the agent of such a session (needs --agent-endpoint, --agent-key-file)\n"
                 "  --transport tcp|udp|p2p|relay|host|forwarder  both ends must match (default: udp)\n"
                 "  --forwarder-key HEX  forwarder: the SHA-256 of the forwarder's key (64 hex digits;\n"
                 "                      rcs_forwarder --identity-sha256); --address and --port are the forwarder's\n"
                 "  --token TOKEN       forwarder: this end's token for the room (rcs_forwarder --sign-token); with\n"
                 "                      --viewers every viewer uses it\n"
                 "  --forwarder-link udp|tcp|URL   forwarder: over UDP (default) or TCP to --address and --port, or\n"
                 "                      a WebSocket: ws://HOST:PORT/PATH (its WebSocket entry) or wss://... (a TLS\n"
                 "                      proxy in front of it)\n"
                 "  --forwarder-pin HEX  forwarder over wss://: the SHA-256 of the proxy's certificate, for one\n"
                 "                      this system does not trust (self-signed)\n"
                 "  --relay ADDR:PORT   host: carry the session through rcsdk_host_relay (a stand-in for the\n"
                 "                      host's own server), as a TCP stream, or with --relay-datagram over UDP\n"
                 "  --room NAME         host: the relay's room, one sharer and its viewers; p2p, relay with\n"
                 "                      --signal-relay: the signal room (default: rcsdk)\n"
                 "  --host-max-datagram N   host: the largest datagram this end sends (default: 1200)\n"
                 "  --turn HOST:PORT --turn-user NAME --turn-pass SECRET   relay: the TURN server\n"
                 "  --turn-transport udp|tcp|tls   relay: how to reach the TURN server (default: udp)\n"
                 "  --turn-pin HEX      tls: accept only the certificate with this SHA-256 (64 hex digits,\n"
                 "                      e.g. a self-signed one); without it the system's trust and the name\n"
                 "  --turn-name NAME    tls: the server's name (SNI, and checked without a pin; default: the\n"
                 "                      host of --turn)\n"
                 "  --stun LIST         p2p: STUN servers, comma-separated host:port\n"
                 "  --interface NAME    use only this network interface (e.g. en0): p2p and relay; udp and tcp viewers\n"
                 "  --audio-dscp N      the DSCP sound datagrams carry, 0..63 (default 0: unmarked; EF is 46)\n"
                 "  --keep-listening on|off  share: listen for the next viewer when one leaves (default: on)\n"
                 "  --max-viewers N     share: serve up to N viewers at once (1..256; default 64)\n"
                 "  --bitrate-from quorum|min|main  share, several viewers: the main encoding's bitrate is\n"
                 "                      the highest half of them reach (who falls short gets a lower\n"
                 "                      encoding), the slowest viewer's, or the main viewer's (default: quorum)\n"
                 "  --encodings auto|single|N  share: lower encodings when a viewer needs one (default), one\n"
                 "                      for all, or N (1..3) always\n"
                 "  --layers N          share: temporal layers of each encoding, 1..3 (default 1: none); a viewer\n"
                 "                      that falls short gets the lower ones first (where the encoder makes them)\n"
                 "  --reconnect on|off  view: reconnect when the path breaks (default: on)\n"
                 "  --reconnect-window S     view: give up after S seconds (30; 0: never)\n"
                 "  --migrate on|off    keep the connection when a network changes (default: on)\n"
                 "  --exclude-networks LIST  networks this end does not use, comma-separated: ethernet, wifi,\n"
                 "                      cellular, other, unknown (rcsdk_cli --info lists them)\n"
                 "  --path-policy follow|stay  move to the system's default network, or stay while the one\n"
                 "                      in use works (default: follow)\n"
                 "  --standby           keep a standby path over another network (udp and tcp viewers, p2p, relay)\n"
                 "  --send-text TEXT    send TEXT on data channel 1 once connected (the peer prints it)\n"
                 "  --data-blast MB     send MB MiB on data channel 7 once connected and report the rate\n"
                 "  --signal-out FILE   p2p, relay: write this end's signal there (and print it); one viewer\n"
                 "                      at a time\n"
                 "  --signal-in FILE    p2p, relay: wait for the peer's signal in that file (each new one)\n"
                 "  --signal-relay ADDR:PORT  p2p, relay: carry the signals through a signal room of\n"
                 "                      rcsdk_host_relay (--room), as a host's server would: any number of viewers\n"
                 "  --viewers N         view: run N viewers in this process, each with its own connection\n"
                 "                      (p2p and relay need --signal-relay); prints a line a second for all\n"
                 "  --viewer-interval MS  with --viewers: start one every MS milliseconds (default 20)\n"
                 "  --audio on|off      sound (default: on; both ends must have it on)\n"
                 "  --audio-codec LIST  audio codecs, preferred first, e.g. opus,aac,pcm (default: all, opus first)\n"
                 "  --audio-bitrate BPS share: Opus/AAC bitrate (128000)\n"
                 "  --exclude-own-sound share: leave this program's own sound out\n"
                 "  --audio-capture B   share: auto, sck, tap or wasapi (default: auto)\n"
                 "  --capture-period N  share: frames per capture period (tap only; default 240)\n"
                 "  --no-playback       view: decode the sound but do not play it\n"
                 "  --send-audio microphone[,DEVICE]|host|off   this end's sound: a microphone (DEVICE from\n"
                 "                      --list-audio-devices; default the system's), a 440 Hz tone pushed as the\n"
                 "                      host's sound, or none (default: a sharer its machine's, a viewer none)\n"
                 "  --no-voice-processing  with --send-audio microphone: no echo cancellation or noise suppression\n"
                 "  --receive-audio on|host|off   the other end's sound: played, handed to on_audio only (its level\n"
                 "                      printed), or none (default: a viewer on, a sharer off)\n"
                 "  --list-audio-devices  the sound devices and the microphone permission\n"
                 "  --buffer-margin-us N  the end that plays the other's sound: buffer margin above one period and\n"
                 "                      the jitter (2000)\n"
                 "  --playback-period N the end that plays: output device frames per period (0: default)\n"
                 "  --playback-queue N  the end that plays, Windows: frames kept queued in the device (0: 1.5\n"
                 "                      periods)\n"
                 "  --audio-delay MS    the end that plays: most sound the playback buffer holds (200)\n"
                 "  --seconds N         stop after N seconds (default: until Ctrl+C)\n"
                 "  --report FILE       write final statistics as JSON\n"
                 "  --log LEVEL         print the SDK's log to stderr: error, warning, info or debug\n"
                 "  --session-log LEVEL with --log: the session's (or agent's) own level, e.g. --log error\n"
                 "                      --session-log debug\n";
}

// Picture formats by their command-line names.
constexpr std::pair<rcs_picture_format, const char*> formatNames[] = {
    {RCS_PICTURE_420_8, "420"},     {RCS_PICTURE_420_10, "420-10"},   {RCS_PICTURE_444_8, "444"},
    {RCS_PICTURE_444_10, "444-10"}, {RCS_PICTURE_HDR10_420, "hdr10"}, {RCS_PICTURE_HDR10_444, "hdr10-444"}};

const char* formatName(rcs_picture_format format) {
    for (const auto& [value, name] : formatNames) {
        if (value == format) {
            return name;
        }
    }
    return "none";
}

rcs_picture_format parseFormat(const std::string& name) {
    for (const auto& [value, text] : formatNames) {
        if (name == text) {
            return value;
        }
    }
    throw std::invalid_argument("Unknown picture format " + name + " (420, 420-10, 444, 444-10, hdr10, hdr10-444)");
}

// "420 444" for a mask of picture formats.
std::string formatList(uint32_t mask) {
    std::string list;
    for (const auto& [value, name] : formatNames) {
        if (mask & (1u << value)) {
            list += (list.empty() ? "" : " ") + std::string(name);
        }
    }
    return list.empty() ? "none" : list;
}

void parseFormats(const std::string& list, rcs_picture_format (&formats)[RCS_MAX_PICTURE_FORMATS]) {
    std::fill(std::begin(formats), std::end(formats), 0u);
    std::stringstream names(list);
    size_t count = 0;
    for (std::string name; std::getline(names, name, ',');) {
        if (count == RCS_MAX_PICTURE_FORMATS) {
            throw std::invalid_argument("--formats takes at most " + std::to_string(RCS_MAX_PICTURE_FORMATS));
        }
        formats[count++] = parseFormat(name);
    }
}

// "BT.709 limited, SDR" for a picture's colour.
std::string colorText(const rcs_color& c) {
    const char* matrix = c.matrix == RCS_MATRIX_BT601    ? "BT.601"
                         : c.matrix == RCS_MATRIX_BT2020 ? "BT.2020"
                         : c.matrix == RCS_MATRIX_BT709  ? "BT.709"
                                                         : "RGB";
    const char* range = c.range == RCS_RANGE_FULL ? " full" : c.range == RCS_RANGE_LIMITED ? " limited" : "";
    return std::string(matrix) + range + (c.transfer == RCS_TRANSFER_PQ ? ", PQ BT.2020 (HDR)" : ", SDR");
}

const char* layoutName(rcs_pixel_format format) {
    switch (format) {
    case RCS_PIXEL_BGRA8:
        return "BGRA8";
    case RCS_PIXEL_RGB10A2:
        return "RGB10A2";
    case RCS_PIXEL_NV12:
        return "NV12";
    case RCS_PIXEL_P010:
        return "P010";
    case RCS_PIXEL_NV24:
        return "NV24";
    case RCS_PIXEL_P410:
        return "P410";
    default:
        return "unknown";
    }
}

// Power actions by name (--power-actions, --power-ask).
const char* const powerActionNames[] = {"", "restart", "shut-down", "sleep", "sign-out"};

rcs_power_action powerActionNamed(const std::string& name) {
    for (rcs_power_action action = RCS_POWER_RESTART; action <= RCS_POWER_SIGN_OUT; ++action) {
        if (name == powerActionNames[action]) {
            return action;
        }
    }
    throw std::invalid_argument("A power action is restart, shut-down, sleep or sign-out, not \"" + name + "\"");
}

std::string powerActionList(uint32_t bits) {
    std::string names;
    for (rcs_power_action action = RCS_POWER_RESTART; action <= RCS_POWER_SIGN_OUT; ++action) {
        if (bits & (1u << action)) {
            names += std::string(names.empty() ? "" : ",") + powerActionNames[action];
        }
    }
    return names.empty() ? std::string("none") : names;
}

std::string powerCapabilities() {
    rcs_power_capabilities caps = RCS_POWER_CAPABILITIES_INIT;
    rcs_power_get_capabilities(&caps);
    return std::string("keep awake ") + (caps.keep_awake ? "yes" : "no") + ", actions " + powerActionList(caps.actions) +
           ", wake interfaces " + (caps.interfaces ? "yes" : "no") + ", wake packets " +
           (caps.send_wake ? "yes" : "no") + (caps.message[0] ? std::string(" (") + caps.message + ")" : std::string());
}

void printInterface(const rcs_power_interface& it) {
    char mac[18];
    std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", it.mac[0], it.mac[1], it.mac[2], it.mac[3],
                  it.mac[4], it.mac[5]);
    const char* wake[] = {"unknown", "on", "off"};
    std::cout << "  " << it.name << " (" << (it.link == RCS_POWER_LINK_WIFI ? "Wi-Fi" : "Ethernet") << "): " << mac
              << ", " << it.address << "/" << it.prefix_length << ", broadcast " << it.broadcast << ", wake "
              << wake[it.wake <= 2 ? it.wake : 0] << std::endl;
}

int printWakeInfo() {
    std::cout << "power: " << powerCapabilities() << "\n";
    uint32_t count = 0;
    if (rcs_power_list_interfaces(nullptr, sizeof(rcs_power_interface), 0, &count) != RCS_OK) {
        std::cout << "wake interfaces: " << rcsdk::lastCallError() << std::endl;
        return 1;
    }
    std::vector<rcs_power_interface> list(count);
    rcs_power_list_interfaces(list.data(), sizeof(rcs_power_interface), count, &count);
    std::cout << "interfaces that can wake this machine: " << count << std::endl;
    for (uint32_t i = 0; i < count && i < list.size(); ++i) {
        printInterface(list[i]);
    }
    return 0;
}

// --wake: magic packets to a sleeping machine on this machine's network.
int sendWake(const Arguments& args) {
    rcs_power_wake wake = RCS_POWER_WAKE_INIT;
    unsigned b[6];
    const std::string mac = args.get("--wake");
    if (std::sscanf(mac.c_str(), "%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x", &b[0], &b[1], &b[2], &b[3],
                    &b[4], &b[5]) != 6) {
        throw std::invalid_argument("--wake takes a MAC address, aa:bb:cc:dd:ee:ff");
    }
    for (int i = 0; i < 6; ++i) {
        wake.mac[i] = uint8_t(b[i]);
    }
    const std::string address = args.get("--wake-address");
    wake.address = address.empty() ? nullptr : address.c_str();
    wake.port = args.number("--wake-port", 9);
    const std::string password = args.get("--wake-password");
    if (!password.empty()) {
        if (password.size() != 8 && password.size() != 12) {
            throw std::invalid_argument("--wake-password takes 4 or 6 bytes, as 8 or 12 hex digits");
        }
        wake.password_size = uint32_t(password.size() / 2);
        for (uint32_t i = 0; i < wake.password_size; ++i) {
            wake.password[i] = uint8_t(std::stoul(password.substr(i * 2, 2), nullptr, 16));
        }
    }
    const uint32_t count = args.number("--wake-count", 3);
    for (uint32_t i = 0; i < count; ++i) {
        if (i) {
            std::this_thread::sleep_for(1s);
        }
        if (rcs_power_send_wake(&wake) != RCS_OK) {
            std::cerr << "The wake packet was not sent: " << rcsdk::lastCallError() << "\n";
            return 1;
        }
        std::cout << "wake packet " << (i + 1) << " sent to " << mac << " via "
                  << (address.empty() ? "255.255.255.255" : address) << ":" << wake.port << std::endl;
    }
    return 0;
}

int printInfo() {
    const auto caps = rcsdk::capabilities();
    const auto codecs = [](uint32_t mask) {
        const std::pair<rcs_codec, const char*> known[] = {{RCS_CODEC_H264, "H.264"}, {RCS_CODEC_HEVC, "HEVC"},
                                                           {RCS_CODEC_VP8, "VP8"},   {RCS_CODEC_VP9, "VP9"},
                                                           {RCS_CODEC_AV1, "AV1"}};
        std::string names;
        for (const auto& [codec, name] : known) {
            if (rcsdk::supports(mask, codec)) {
                names += std::string(names.empty() ? "" : " ") + name;
            }
        }
        return names.empty() ? std::string("none") : names;
    };
    std::string sources;
    const char* sourceNames[] = {"display", "region", "window", "application", "host", "encoded"};
    for (uint32_t source = RCS_CAPTURE_DISPLAY; source <= RCS_CAPTURE_ENCODED; ++source) {
        if (rcsdk::supports(caps.capture_sources, source)) {
            sources += std::string(sources.empty() ? "" : " ") + sourceNames[source];
        }
    }
    std::string inputs;
    const char* inputNames[] = {"",    "move", "relative", "button", "wheel",       "scroll",
                                "key", "text", "touch",    "pen",    "release-all", "ctrl-alt-del"};
    for (uint32_t type = RCS_INPUT_MOVE; type <= RCS_INPUT_SECURE_ATTENTION; ++type) {
        if (caps.input_types & (1u << type)) {
            inputs += std::string(inputs.empty() ? "" : " ") + inputNames[type];
        }
    }
    std::cout << "SDK " << rcs_version() << "\n"
              << "capture sources: " << sources << "\n"
              << "input: " << (inputs.empty() ? "none" : inputs) << "\n"
              << "clipboard formats: 0x" << std::hex << rcs_clipboard_system_formats() << std::dec << "\n"
              << "text caret: " << (rcs_text_caret_available() ? "yes" : "no") << "\n"
              << "power: " << powerCapabilities() << "\n"
              << "hardware encoders: " << codecs(caps.hardware_encoders)
              << ", decoders: " << codecs(caps.hardware_decoders) << "\n"
              << "software encoders: " << codecs(caps.software_encoders)
              << ", decoders: " << codecs(caps.software_decoders)
              << (caps.software_decoding_gpu_frames ? " (GPU frames)" : " (CPU frames)") << "\n"
              << "permissions: screen " << rcs_screen_capture_permission() << ", input " << rcs_input_permission()
              << " (2 = granted)\n"
              << "capture picture formats: " << formatList(caps.picture_formats) << "\n"
              << "Wi-Fi out of power saving while connected: " << (caps.wifi_low_latency ? "yes" : "no") << "\n";
    for (const auto& i : rcsdk::codecImplementations()) {
        std::cout << (i.direction == RCS_CODEC_ENCODER ? "encoder " : "decoder ")
                  << (i.acceleration == RCS_ACCELERATION_HARDWARE ? "hardware " : "software ") << i.name << " ["
                  << formatList(i.picture_formats) << "]";
        if (i.backend == RCS_BACKEND_NVENC) {
            std::cout << " [nvenc]";
        }
        if (i.adapter_luid) {
            std::cout << " (GPU " << std::hex << i.adapter_luid << std::dec << ")";
        }
        for (rcs_picture_format format = RCS_PICTURE_420_8; format <= RCS_PICTURE_HDR10_444; ++format) {
            if (const uint64_t rate = i.max_pixels_per_second[format]) {
                std::cout << " " << formatList(1u << format) << " 4K at most " << rate / (3840 * 2160) << " FPS";
            }
        }
        std::cout << "\n";
    }
    for (const auto& d : rcsdk::displays()) {
        std::cout << "display " << d.display_id << ": " << d.pixel_width << "x" << d.pixel_height
                  << (d.rotation_degrees ? ", rotation " + std::to_string(d.rotation_degrees) : std::string())
                  << ", at " << d.desktop_x << "," << d.desktop_y << " (" << d.desktop_width << "x"
                  << d.desktop_height << " on the desktop)" << (d.is_main ? " (main)" : "") << "\n";
    }
    if (caps.transports & (1u << RCS_TRANSPORT_UDP)) {
        static const char* const kinds[] = {"unknown", "Ethernet", "Wi-Fi", "cellular", "other"};
        static const char* const flags[] = {"metered", "constrained", "default", "VPN"};
        for (const auto& n : rcsdk::networks()) {
            std::cout << "network " << n.name << " (index " << n.index << "): " << kinds[n.kind]
                      << (n.ipv4 ? " IPv4" : "") << (n.ipv6 ? " IPv6" : "");
            for (uint32_t bit = 0; bit < 4; ++bit) {
                std::cout << ((n.flags & (1u << bit)) ? std::string(" ") + flags[bit] : std::string());
            }
            std::cout << "\n";
        }
        static const char* const bands[] = {"?", "2.4 GHz", "5 GHz", "6 GHz"};
        for (const auto& w : rcsdk::wifi()) {
            std::cout << "wifi " << w.name << ": " << bands[w.band <= RCS_WIFI_BAND_6_GHZ ? w.band : 0] << " channel "
                      << w.channel << ", " << w.width_mhz << " MHz, signal " << w.signal_dbm << " dBm, noise "
                      << w.noise_dbm << " dBm, " << w.rate_mbps << " Mbit/s"
                      << (w.peer_to_peer ? ", AWDL up" : "")
                      << (w.other_channel ? " on another channel: stalls about every 1.05 s (use channel 6, 44 or 149)"
                                          : "")
                      << "\n";
        }
    }
    return 0;
}

int printWindows() {
    for (const auto& w : rcsdk::windows()) {
        std::cout << "window 0x" << std::hex << w.window_id << std::dec << " process " << w.process_id << " display "
                  << w.display_id << " " << w.x << "," << w.y << " " << w.width << "x" << w.height
                  << (w.minimized ? " minimized" : "") << (w.focused ? " focused" : "") << "  " << w.application
                  << (w.title[0] ? std::string(": ") + w.title : std::string()) << "\n";
    }
    return 0;
}

// What --display, --region, --window or --application name (the main display without them).
rcs_capture_config captureOf(const Arguments& args) {
    rcsdk::SessionConfig config(RCS_ROLE_SHARE);
    const uint32_t display = args.number("--display", 0);
    if (args.has("--region")) {
        uint32_t x = 0, y = 0, width = 0, height = 0;
        if (std::sscanf(args.get("--region").c_str(), "%u,%u,%u,%u", &x, &y, &width, &height) != 4) {
            throw std::invalid_argument("--region takes X,Y,W,H");
        }
        config.setCaptureRegion(display, x, y, width, height);
    } else if (args.has("--window")) {
        config.setCaptureWindow(std::stoull(args.get("--window"), nullptr, 0));
    } else if (args.has("--application")) {
        config.setCaptureApplication(args.number("--application", 0), display);
    } else {
        config.setCaptureDisplay(display);
    }
    return config.capture();
}

// BGRA8 pixels (rows of width * 4 bytes) written as a 32-bit .bmp (rows
// bottom up, as BMP keeps them).
void writeBmp(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& pixels) {
    const uint32_t imageSize = width * height * 4;
    const auto put = [](std::vector<uint8_t>& to, uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            to.push_back(uint8_t(value >> (8 * i)));
        }
    };
    std::vector<uint8_t> file{'B', 'M'};
    put(file, 54 + imageSize, 4);
    put(file, 0, 4);
    put(file, 54, 4);
    put(file, 40, 4); // BITMAPINFOHEADER
    put(file, width, 4);
    put(file, height, 4);
    put(file, 1, 2);
    put(file, 32, 2);
    put(file, 0, 4); // BI_RGB
    put(file, imageSize, 4);
    put(file, 2835, 4);
    put(file, 2835, 4);
    put(file, 0, 4);
    put(file, 0, 4);
    for (uint32_t row = height; row-- > 0;) {
        const auto* line = pixels.data() + size_t(row) * width * 4;
        file.insert(file.end(), line, line + size_t(width) * 4);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()))) {
        throw std::runtime_error("Could not write " + path);
    }
}

// A thumbnail written as a .bmp.
int writeThumbnail(const Arguments& args) {
    uint32_t maxWidth = 320, maxHeight = 320;
    if (args.has("--max-size") && std::sscanf(args.get("--max-size").c_str(), "%u,%u", &maxWidth, &maxHeight) != 2) {
        throw std::invalid_argument("--max-size takes W,H");
    }
    const auto started = std::chrono::steady_clock::now();
    const auto picture = rcsdk::capturePicture(captureOf(args), maxWidth, maxHeight);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    writeBmp(args.get("--thumbnail"), picture.width, picture.height, picture.pixels);
    std::cout << picture.width << "x" << picture.height << " in " << took.count() << " ms -> "
              << args.get("--thumbnail") << "\n";
    return 0;
}

std::string hexOf(const uint8_t* bytes, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 15];
    }
    return text;
}

rcsdk::PublicKey publicKeyFromHex(const std::string& text) {
    rcsdk::PublicKey key{};
    if (text.size() != key.size() * 2) {
        throw std::invalid_argument("--trust takes public keys of 130 hex digits (see --generate-device-key)");
    }
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = uint8_t(std::stoul(text.substr(i * 2, 2), nullptr, 16));
    }
    return key;
}

// A device key in a file of its own; its public key, to give to peers, on stdout.
int generateDeviceKey(const std::string& path) {
    const auto key = rcsdk::generateDeviceKey();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(key.data()), key.size());
    const auto publicKey = rcsdk::publicKeyOf(key);
    std::cout << "Wrote a new device key to " << path << "; its public key (for --trust on the peer):\n"
              << hexOf(publicKey.data(), publicKey.size()) << "\n";
    return 0;
}

int generateKey(const std::string& path) {
    uint8_t key[32];
    if (rcs_generate_connection_key(key, sizeof(key)) != RCS_OK) {
        return 1;
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(key), sizeof(key));
    std::cout << "Wrote a new 32-byte key to " << path << "\n";
    return 0;
}

// The host codec of --host-codec and --encoded-pattern (see RawEncoder).
constexpr rcs_codec rawCodec = RCS_CODEC_HOST_FIRST;
constexpr uint32_t rawScale = 16;

const char* codecName(rcs_codec codec) {
    switch (codec) {
    case RCS_CODEC_H264:
        return "h264";
    case RCS_CODEC_HEVC:
        return "hevc";
    case RCS_CODEC_VP8:
        return "vp8";
    case RCS_CODEC_VP9:
        return "vp9";
    case RCS_CODEC_AV1:
        return "av1";
    case rawCodec:
        return "host16";
    default:
        return "none";
    }
}

// "hevc,h264" -> {RCS_CODEC_HEVC, RCS_CODEC_H264}; an empty list keeps the default.
void parseCodecs(const std::string& list, rcs_codec (&codecs)[RCS_MAX_CODECS]) {
    if (list.empty()) {
        return;
    }
    std::fill(std::begin(codecs), std::end(codecs), 0u);
    std::stringstream stream(list);
    size_t count = 0;
    for (std::string name; std::getline(stream, name, ',');) {
        rcs_codec codec = 0;
        for (const rcs_codec candidate :
             {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_VP8, RCS_CODEC_VP9, RCS_CODEC_AV1}) {
            codec = name == codecName(candidate) ? candidate : codec;
        }
        if (!codec || count == RCS_MAX_CODECS) {
            throw std::invalid_argument("--codec takes a list of h264, hevc, vp8, vp9, av1");
        }
        codecs[count++] = codec;
    }
}

const char* audioName(rcs_audio_codec codec) {
    switch (codec) {
    case RCS_AUDIO_OPUS:
        return "opus";
    case RCS_AUDIO_AAC:
        return "aac";
    case RCS_AUDIO_PCM:
        return "pcm";
    default:
        return "none";
    }
}

void parseAudioCodecs(const std::string& list, rcs_audio_codec (&codecs)[RCS_MAX_AUDIO_CODECS]) {
    std::fill(std::begin(codecs), std::end(codecs), 0u);
    std::stringstream stream(list);
    std::string name;
    size_t count = 0;
    while (std::getline(stream, name, ',')) {
        const rcs_audio_codec codec = name == "opus" ? RCS_AUDIO_OPUS
                                      : name == "aac" ? RCS_AUDIO_AAC
                                      : name == "pcm" ? RCS_AUDIO_PCM
                                                      : 0;
        if (!codec || count == RCS_MAX_AUDIO_CODECS) {
            throw std::invalid_argument("--audio-codec takes up to three of opus, aac, pcm");
        }
        codecs[count++] = codec;
    }
}

// A sharer's viewers as JSON, each with its own connection's statistics.
std::string viewersJson(const rcsdk::Session& session) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& viewer : session.viewers()) {
        const auto network = session.viewerStats<rcs_network_stats>(viewer.viewer_id);
        const auto audio = session.viewerStats<rcs_audio_send_stats>(viewer.viewer_id);
        const auto input = session.viewerStats<rcs_input_stats>(viewer.viewer_id);
        const auto data = session.viewerStats<rcs_data_stats>(viewer.viewer_id);
        out << (first ? "" : ",") << "\n    {\"id\": " << viewer.viewer_id << ", \"address\": \"" << viewer.address
            << "\", \"port\": " << viewer.port << ", \"main\": " << viewer.is_main
            << ", \"platform\": " << viewer.peer.platform << ", \"control\": " << viewer.control.granted
            << ", \"path\": \"" << network.path << "\", \"round_trip_ms\": " << network.round_trip_ns / 1e6
            << ", \"received_bps\": " << network.received_bps << ", \"loss_permille\": " << network.loss_permille
            << ", \"queue_delay_ms\": " << network.queue_delay_us / 1e3 << ", \"bytes\": " << network.bytes
            << ", \"rate_reports\": " << network.rate_reports << ", \"audio_packets\": " << audio.encoded_packets
            << ", \"input_received\": " << input.received << ", \"data_sent\": " << data.sent_messages
            << ", \"data_received\": " << data.received_messages << "}";
        first = false;
    }
    out << (first ? "]" : "\n  ]");
    return out.str();
}

void writeReport(const std::string& path, const rcsdk::Session& session, bool share, double seconds, uint64_t frames,
                 const std::string& viewers) {
    // Sections of the other role stay zero.
    const auto capture = session.stats<rcs_capture_stats>();
    const auto sent = session.stats<rcs_video_send_stats>();
    const auto received = session.stats<rcs_video_receive_stats>();
    const auto network = session.stats<rcs_network_stats>();
    const auto input = session.stats<rcs_input_stats>();
    const auto audioSent = session.stats<rcs_audio_send_stats>();
    const auto audioReceived = session.stats<rcs_audio_receive_stats>();
    rcs_clipboard_stats clipboard = RCS_CLIPBOARD_STATS_INIT; // Zero where the session does not use it.
    rcs_clipboard_get_stats(session.handle(), 0, &clipboard);
    const auto stream = session.streamInfo();
    const auto peer = session.peerInfo();
    const auto failure = session.failure();
    std::ofstream out(path);
    out << "{\n"
        << "  \"version\": \"" << rcs_version() << "\",\n"
        << "  \"state\": " << session.state() << ",\n"
        << "  \"result\": " << failure.result << ",\n"
        << "  \"error\": \"" << failure.message << "\",\n"
        << "  \"seconds\": " << seconds << ",\n"
        << "  \"frames\": " << frames << ",\n"
        << "  \"fps\": " << (seconds > 0 ? frames / seconds : 0) << ",\n"
        << "  \"stream\": {\"codec\": \"" << codecName(stream.codec) << "\", \"format\": \""
        << formatName(stream.format) << "\", \"color\": \"" << colorText(stream.color)
        << "\", \"hdr_max_luminance\": " << stream.hdr.max_luminance << ", \"width\": " << stream.width
        << ", \"height\": " << stream.height << ", \"fps\": " << stream.frames_per_second
        << ", \"bitrate_bps\": " << stream.bitrate_bps << ", \"encoder_acceleration\": "
        << (stream.encoder_acceleration == RCS_ACCELERATION_SOFTWARE ? "\"software\"" : "\"hardware\"") << "},\n"
        << "  \"peer\": {\"platform\": " << peer.platform << ", \"codecs\": " << peer.codecs
        << ", \"send_audio_codecs\": " << peer.send_audio_codecs << ", \"receive_audio_codecs\": "
        << peer.receive_audio_codecs << ", \"sends_audio\": " << peer.sends_audio
        << ", \"receives_audio\": " << peer.receives_audio << "},\n"
        << "  \"codec_implementation\": \"" << (share ? sent.encoder : received.decoder) << "\",\n"
        << "  \"bitrate_bps\": " << sent.bitrate_bps << ", \"round_trip_ms\": " << network.round_trip_ns / 1e6
        << ", \"loss_permille\": " << network.loss_permille << ", \"path\": \"" << network.path << "\",\n"
        << "  \"rate_reports\": " << network.rate_reports << ", \"rate_overrides\": " << network.rate_overrides << ",\n"
        << "  \"fec_percent\": " << network.fec_percent << ", \"fec_sent\": " << network.fec_sent
        << ", \"fec_recovered\": " << network.fec_recovered << ", \"resend_recovered\": " << network.resend_recovered
        << ", \"frames_lost\": " << network.frames_lost << ", \"marked_packets\": " << network.marked_packets
              << ", \"backlog_drops\": " << network.backlog_drops << ", \"path_bps\": " << network.path_bps
        << ", \"sound_wait_us\": " << network.sound_wait_us << ", \"sound_queue_us\": " << network.sound_queue_us
        << ",\n"
        << "  \"encoder_low_latency\": " << sent.low_latency << ", \"capture_interruptions\": " << capture.interruptions
        << ", \"connections\": " << network.connections
        << ", \"rejected_connections\": " << network.rejected_connections
        << ", \"pin_failures\": " << network.pin_failures << ", \"decoder_recoveries\": " << received.decoder_recoveries
        << ", \"wifi_low_latency\": " << network.wifi_low_latency << ",\n"
        << "  \"stalls\": " << network.stalls << ", \"stall_ms\": " << network.stall_ms
        << ", \"longest_stall_ms\": " << network.longest_stall_ms << ", \"stall_period_ms\": " << network.stall_period_ms
        << ", \"peer_stall_period_ms\": " << network.peer_stall_period_ms << ", \"peer_stall_ms\": " << network.peer_stall_ms
        << ",\n"
        << "  \"input_received\": " << input.received << ", \"input_applied\": " << input.applied
        << ", \"input_sent\": " << input.sent << ", \"last_input_result\": " << input.last_result << ",\n"
        << "  \"captured_frames\": " << capture.captured_frames
        << ", \"capture_overwrites\": " << capture.overwritten_frames << ",\n"
        << "  \"encoded_frames\": " << sent.encoded_frames << ", \"encoded_bytes\": " << sent.encoded_bytes
        << ", \"size_percent\": " << sent.size_percent << ", \"limited_ms\": " << sent.limited_ms
        << ", \"decoded_frames\": " << received.decoded_frames
        << ",\n"
        << "  \"network_bytes\": " << network.bytes << ", \"dropped_packets\": " << received.dropped_packets
        << ", \"recovery_requests\": " << received.recovery_requests << ",\n"
        << "  \"last_encode_ms\": " << sent.last_encode_ns / 1e6
        << ", \"last_decode_ms\": " << received.last_decode_ns / 1e6
        << ", \"last_pipeline_ms\": " << (share ? sent.last_pipeline_ns : received.last_pipeline_ns) / 1e6 << ",\n"
        << "  \"audio\": {\"codec\": \"" << audioName(stream.audio_codec)
        << "\", \"active\": " << (share ? audioSent.active : audioReceived.active) << ", \"implementation\": \""
        << (share ? audioSent.encoder : audioReceived.decoder) << "\", \"device\": \"" << audioReceived.device
        << "\", \"capture_backend\": " << audioSent.capture_backend
        << ", \"capture_period_frames\": " << audioSent.capture_period_frames
        << ", \"playback_period_frames\": " << audioReceived.playback_period_frames
        << ", \"playback_queue_frames\": " << audioReceived.playback_queue_frames
        << ", \"packets\": " << (share ? audioSent.encoded_packets : audioReceived.received_packets)
        << ", \"bytes\": " << audioSent.encoded_bytes << ", \"interruptions\": " << audioSent.interruptions
        << ", \"lost_packets\": " << audioReceived.lost_packets
        << ", \"recovered_packets\": " << audioReceived.recovered_packets
        << ", \"concealed_ms\": " << audioReceived.concealed_ms
        << ", \"dropped_ms\": " << audioReceived.dropped_ms << ", \"underruns\": " << audioReceived.underruns
        << ", \"device_underruns\": " << audioReceived.device_underruns
        << ", \"buffer_ms\": " << audioReceived.buffer_ms << ", \"target_ms\": " << audioReceived.target_ms << "},\n"
        << "  \"clipboard\": {\"sent\": " << clipboard.sent << ", \"received\": " << clipboard.received
        << ", \"refused\": " << clipboard.refused << ", \"concealed\": " << clipboard.concealed
        << ", \"failed\": " << clipboard.failed << "},\n"
        << "  \"viewers\": " << (share ? viewers : "[]") << "\n"
        << "}\n";
}

// --host-pattern: pictures of the stream's size pushed at its frame rate as the
// host's own frames: bars sliding by, and grain no encoder can predict in a
// quarter of the picture. A load on the path without a screen.
// --host-codec: a codec of the hosts' own (host codec 16), as an application
// with an encoder and decoder of its own brings one: each picture shrunk 16
// times each way, three bytes a pixel, every packet a keyframe. It shows the
// host codec interfaces at work, not a good picture.

std::vector<uint8_t> shrink(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride) {
    std::vector<uint8_t> out;
    out.reserve(size_t(width / rawScale) * (height / rawScale) * 3);
    for (uint32_t y = 0; y < height / rawScale; ++y) {
        for (uint32_t x = 0; x < width / rawScale; ++x) {
            const uint8_t* p = bgra + size_t(y * rawScale + rawScale / 2) * stride + (x * rawScale + rawScale / 2) * 4;
            out.insert(out.end(), p, p + 3);
        }
    }
    return out;
}

rcs_encoded_packet rawPacket(const std::vector<uint8_t>& data, uint32_t width, uint32_t height, uint64_t sequence,
                             uint64_t timestampNs) {
    rcs_encoded_packet packet = RCS_ENCODED_PACKET_INIT; // 4:2:0 8-bit pictures, BT.709, limited range.
    packet.codec = rawCodec;
    packet.width = width;
    packet.height = height;
    packet.keyframe = 1;
    packet.sequence = sequence;
    packet.timestamp_ns = timestampNs;
    packet.data = data.data();
    packet.data_size = data.size();
    return packet;
}

// Takes BGRA8 on the CPU (cpu_images): captures on the GPU are read back by the SDK.
class RawEncoder final : public rcsdk::HostEncoder {
public:
    // Each stream opens its own (rcs_streams_config), from its own thread.
    rcs_result open(const rcs_host_encoder_settings& settings) override {
        settings_[settings.stream] = settings;
        return RCS_OK;
    }
    rcs_result encode(const rcs_host_encoder_frame& picture) override {
        const rcs_frame& p = *picture.cpu_frame;
        const auto data = shrink(p.data, p.width, p.height, p.stride_bytes);
        return rcsdk::pushEncoded(settings_[picture.stream].output,
                                  rawPacket(data, p.width, p.height, picture.sequence, picture.timestamp_ns));
    }
    void close(uint32_t, uint32_t) override {}

private:
    std::array<rcs_host_encoder_settings, RCS_MAX_STREAMS> settings_{};
};

class RawDecoder final : public rcsdk::HostDecoder {
public:
    std::atomic<uint64_t> packets{0}, wrongSize{0};
    std::atomic<uint32_t> colour{0}; // Mean B, G, R of the latest, from the low byte up.

    // Each stream opens its own (rcs_streams_config), from its own thread.
    rcs_result open(const rcs_host_decoder_settings& settings) override {
        expected_[settings.stream] = size_t(settings.width / rawScale) * (settings.height / rawScale) * 3;
        return RCS_OK;
    }
    rcs_result decode(const rcs_encoded_packet& packet) override {
        if (packet.data_size != expected_[packet.stream]) {
            ++wrongSize;
            return RCS_ERROR_CODEC;
        }
        uint64_t sum[3] = {};
        for (size_t i = 0; i < packet.data_size; ++i) {
            sum[i % 3] += packet.data[i];
        }
        const uint64_t pixels = packet.data_size / 3;
        colour = uint32_t(sum[0] / pixels) | uint32_t(sum[1] / pixels) << 8 | uint32_t(sum[2] / pixels) << 16;
        ++packets;
        return RCS_OK;
    }
    void reset(uint32_t) override {}
    void close(uint32_t) override {}

private:
    std::array<size_t, RCS_MAX_STREAMS> expected_{};
};

// --encoded-pattern: the sharer's own bitstream in the raw codec, pushed at the
// stream's frame rate while the path has room (capture source ENCODED).
class EncodedPattern {
public:
    explicit EncodedPattern(rcsdk::Session& session) : session_(session), thread_([this] { run(); }) {}
    ~EncodedPattern() {
        stop_ = true;
        thread_.join();
    }
    uint64_t pushed() const noexcept { return pushed_; }
    uint64_t skipped() const noexcept { return skipped_; }

private:
    void run() {
        uint64_t frame = 0;
        auto next = std::chrono::steady_clock::now();
        while (!stop_) {
            const auto info = session_.streamInfo();
            next += std::chrono::microseconds(1'000'000 / std::max(info.frames_per_second, 1u));
            std::this_thread::sleep_until(next);
            if (rcsdk::videoReady(session_) != RCS_OK) {
                skipped_ += info.width != 0;
                continue;
            }
            const uint32_t w = info.width / rawScale, h = info.height / rawScale;
            std::vector<uint8_t> data(size_t(w) * h * 3);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t* p = data.data() + (size_t(y) * w + x) * 3;
                    p[0] = ((x + frame) / 4) % 2 ? 200 : 40;
                    p[1] = uint8_t(y * 255 / std::max(h, 1u));
                    p[2] = uint8_t(frame * 3);
                }
            }
            const auto packet = rawPacket(data, info.width, info.height, ++frame, 0);
            pushed_ += rcsdk::pushEncodedVideo(session_, packet) == RCS_OK;
        }
    }

    rcsdk::Session& session_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> pushed_{0}, skipped_{0};
    std::thread thread_;
};

// The marker the viewer's Space advances (--pattern-marker).
std::atomic<uint32_t> patternMarker{0};

class PatternSource {
public:
    PatternSource(rcsdk::Session& session, bool marker)
        : session_(session), marker_(marker), thread_([this] { run(); }) {}
    ~PatternSource() {
        stop_ = true;
        thread_.join();
    }

private:
    void run() {
        std::vector<uint8_t> pixels;
        uint32_t noise = 0x12345678, frame = 0;
        auto next = std::chrono::steady_clock::now();
        while (!stop_) {
            const auto info = session_.streamInfo();
            next += std::chrono::microseconds(1'000'000 / std::max(info.frames_per_second, 1u));
            std::this_thread::sleep_until(next);
            if (!info.width || !info.height) {
                continue;
            }
            pixels.resize(size_t(info.width) * info.height * 4);
            if (marker_) {
                drawMarker(pixels, info.width, info.height, frame);
            } else {
                drawGrain(pixels, info.width, info.height, frame, noise);
            }
            session_.pushVideoFrame(pixels.data(), info.width, info.height, info.width * 4);
            ++frame;
        }
    }

    // Bars sliding by (one row, copied: cheap enough for 4K at 120 FPS) and
    // the probe's marker across the middle, as rcsdk_latency_target draws it.
    static void drawMarker(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t frame) {
        const size_t stride = size_t(width) * 4;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = pixels.data() + size_t(x) * 4;
            const bool bar = ((x + frame * 8) / 64) % 2;
            p[0] = bar ? 200 : 40;
            p[1] = 90;
            p[2] = bar ? 60 : 180;
            p[3] = 255;
        }
        for (uint32_t y = 1; y < height; ++y) {
            std::memcpy(pixels.data() + y * stride, pixels.data(), stride);
        }
        const uint32_t value = marker::bits(uint8_t(patternMarker.load()));
        for (uint32_t y = uint32_t(height * .42); y < uint32_t(height * .58); ++y) {
            uint8_t* row = pixels.data() + y * stride;
            for (int i = 0; i < 32; ++i) {
                const uint8_t level = (value & (1u << (31 - i))) ? 255 : 0;
                const auto from = uint32_t(width * (.2 + i * .6 / 32)), to = uint32_t(width * (.2 + (i + 1) * .6 / 32));
                std::memset(row + size_t(from) * 4, level, size_t(to - from) * 4);
            }
        }
    }

    // Bars sliding by, and grain no encoder can predict in a quarter.
    static void drawGrain(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t frame,
                          uint32_t& noise) {
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* row = pixels.data() + size_t(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t* p = row + size_t(x) * 4;
                if (x < width / 2 && y < height / 2) {
                    noise ^= noise << 13;
                    noise ^= noise >> 17;
                    noise ^= noise << 5;
                    p[0] = p[1] = p[2] = uint8_t(noise);
                } else {
                    const bool bar = ((x + frame * 8) / 64) % 2;
                    p[0] = bar ? 200 : 40;
                    p[1] = uint8_t(y * 255 / height);
                    p[2] = bar ? 60 : 180;
                }
                p[3] = 255;
            }
        }
    }

    rcsdk::Session& session_;
    const bool marker_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// --rate-cap, --rate-trace: adaptive bitrate decided by the host (a ceiling of
// its own over the rules' proposal), and a trace of each report as CSV.
class RateCeiling final : public rcsdk::RateDecider {
public:
    RateCeiling(uint32_t capBps, const std::string& tracePath) : cap_(capBps) {
        if (!tracePath.empty()) {
            trace_.open(tracePath);
            trace_ << "time_ms,queue_ms,loss_permille,received_kbps,round_trip_ms,encoder_kbps,backlog_ms,current_kbps,"
                      "proposal_kbps,decision_kbps\n";
        }
    }
    void decide(const rcs_rate_report& r, rcs_rate_decision& d) override {
        const uint32_t proposal = d.bitrate_bps;
        if (cap_) {
            d.bitrate_bps = std::min(d.bitrate_bps, cap_);
        }
        if (trace_.is_open()) {
            started_ = started_ ? started_ : r.timestamp_ns;
            trace_ << (r.timestamp_ns - started_) / 1'000'000 << "," << r.queue_delay_us / 1000.0 << ","
                   << r.loss_permille << "," << r.received_bps / 1000 << "," << r.round_trip_ns / 1'000'000.0 << ","
                   << r.encoder_bps / 1000 << "," << r.send_backlog_ns / 1'000'000.0 << "," << r.current_bps / 1000
                   << "," << proposal / 1000 << "," << d.bitrate_bps / 1000 << "\n";
        }
    }

private:
    const uint32_t cap_;
    std::ofstream trace_;
    uint64_t started_ = 0;
};

rcs_preset presetOf(const Arguments& args) {
    static const std::map<std::string, rcs_preset> presets = {{"balanced", RCS_PRESET_BALANCED},
                                                              {"low-latency", RCS_PRESET_LOW_LATENCY},
                                                              {"low-bandwidth", RCS_PRESET_LOW_BANDWIDTH},
                                                              {"quality", RCS_PRESET_QUALITY}};
    const auto preset = presets.find(args.get("--preset", "balanced"));
    if (preset == presets.end()) {
        throw std::invalid_argument("--preset takes balanced, low-latency, low-bandwidth or quality");
    }
    return preset->second;
}

rcs_log_level logLevel(const Arguments& args, const std::string& option) {
    static const std::map<std::string, rcs_log_level> levels = {
        {"error", RCS_LOG_ERROR}, {"warning", RCS_LOG_WARNING}, {"info", RCS_LOG_INFO}, {"debug", RCS_LOG_DEBUG}};
    const auto level = levels.find(args.get(option));
    if (level == levels.end()) {
        throw std::invalid_argument(option + " takes error, warning, info or debug");
    }
    return level->second;
}

// A publication's id as 32 hex digits, and back (empty where it is not one).
std::string publicationHex(const uint8_t* id) {
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (uint32_t i = 0; i < RCS_FILE_PUBLICATION_SIZE; ++i) {
        hex += digits[id[i] >> 4];
        hex += digits[id[i] & 15];
    }
    return hex;
}

std::vector<uint8_t> publicationOf(const std::string& hex) {
    std::vector<uint8_t> id;
    for (size_t i = 0; hex.size() == 2 * RCS_FILE_PUBLICATION_SIZE && i < hex.size(); i += 2) {
        id.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return id;
}

// --fetch-read: a READ fetch's files, each read from its start to its end
// (16 reads asked ahead) and written below a directory as they come, as a
// system paste would read them; closed once all are.
class FetchReader {
public:
    FetchReader(rcs_session session, std::string directory) : session_(session), directory_(std::move(directory)) {}
    void into(rcs_file_fetch_request& request) {
        request.user_data = this;
        request.on_listed = [](uint64_t transfer, void* self) { static_cast<FetchReader*>(self)->listed(transfer); };
        request.on_read = [](uint64_t, uint64_t tag, rcs_result result, const uint8_t* data, uint32_t size,
                             void* self) { static_cast<FetchReader*>(self)->answered(tag, result, data, size); };
        request.on_ended = [](uint64_t transfer, rcs_result result, const char* message, void*) {
            std::cout << "fetch " << transfer << " read ended: " << rcs_result_message(result)
                      << (message[0] ? std::string(" (") + message + ")" : std::string()) << std::endl;
        };
    }

private:
    void listed(uint64_t transfer) {
        transfer_ = transfer;
        uint32_t count = 0;
        rcs_file_get_items(session_, transfer, 0, nullptr, sizeof(rcs_file_item_info), 0, &count);
        items_.resize(count);
        rcs_file_get_items(session_, transfer, 0, items_.data(), sizeof(rcs_file_item_info), count, &count);
        std::cout << "fetch " << transfer << ": " << count << " items listed, reading them" << std::endl;
        next();
    }
    // The next item: directories made, files opened and read.
    void next() {
        for (; item_ < items_.size(); ++item_) {
            const auto& item = items_[item_];
            const std::filesystem::path path =
                std::filesystem::path(directory_) / std::u8string(reinterpret_cast<const char8_t*>(item.path));
            if (item.kind == RCS_FILE_DIRECTORY) {
                std::filesystem::create_directories(path);
            } else if (item.kind == RCS_FILE_REGULAR) {
                std::filesystem::create_directories(path.parent_path());
                out_ = std::ofstream(path, std::ios::binary | std::ios::trunc);
                asked_ = 0;
                if (item.size) {
                    ask();
                    return;
                }
            }
        }
        const rcs_result closed = rcs_file_close(session_, transfer_, RCS_OK, nullptr);
        std::cout << "fetch " << transfer_ << ": read " << bytes_ << " bytes of " << items_.size() << " items into "
                  << directory_ << "; closed: " << rcs_result_message(closed) << std::endl;
    }
    void ask() {
        const uint64_t size = items_[item_].size;
        while (asked_ < size && pending_.size() < 16) {
            const auto length = uint32_t(std::min<uint64_t>(RCS_FILE_READ_MAX, size - asked_));
            if (rcs_file_read(session_, transfer_, item_, asked_, length, asked_) != RCS_OK) {
                std::cout << "fetch " << transfer_ << ": rcs_file_read: " << rcsdk::lastCallError() << std::endl;
                return;
            }
            pending_.push_back(asked_);
            asked_ += length;
        }
    }
    void answered(uint64_t tag, rcs_result result, const uint8_t* data, uint32_t size) {
        if (pending_.empty() || pending_.front() != tag) {
            return; // Answered as the fetch ended.
        }
        pending_.pop_front();
        if (result != RCS_OK) {
            std::cout << "fetch " << transfer_ << ": " << items_[item_].path << " at " << tag << ": "
                      << rcs_result_message(result) << std::endl;
            rcs_file_close(session_, transfer_, result, "a read failed");
            return;
        }
        out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
        bytes_ += size;
        if (pending_.empty() && asked_ == items_[item_].size) {
            out_.close();
            ++item_;
            next();
        } else {
            ask();
        }
    }

    const rcs_session session_;
    const std::string directory_;
    uint64_t transfer_ = 0;
    std::vector<rcs_file_item_info> items_;
    uint32_t item_ = 0;
    uint64_t asked_ = 0, bytes_ = 0;
    std::deque<uint64_t> pending_; // Offsets asked, in order.
    std::ofstream out_;
};

// Each line names what wrote it: "[info] session network: Connected".
// --send-file, --receive-dir: file transfers as a host runs them. Its
// decisions are taken in the events (a host with a window would ask first).
// --publish: what the other end may fetch (its id printed); --fetch,
// --fetch-read: the other end's publication fetched into --receive-dir.
class FileHost {
public:
    explicit FileHost(const Arguments& args)
        : receiveDir_(args.get("--receive-dir")), fetch_(args.get("--fetch")), fetchRead_(args.get("--fetch-read")),
          publishOut_(args.get("--publish-out")) {
        for (const auto& path : args.all("--send-file")) {
            const auto full = std::filesystem::absolute(path).u8string();
            send_.emplace_back(full.begin(), full.end());
        }
        for (const auto& path : args.all("--publish")) {
            const auto full = std::filesystem::absolute(path).u8string();
            publish_.emplace_back(full.begin(), full.end());
        }
        if ((!fetch_.empty() || !fetchRead_.empty()) && receiveDir_.empty()) {
            throw std::invalid_argument("--fetch and --fetch-read need --receive-dir");
        }
        const std::string conflict = args.get("--file-conflict", "rename");
        conflict_ = conflict == "overwrite" ? RCS_FILE_CONFLICT_OVERWRITE
                    : conflict == "rename"  ? RCS_FILE_CONFLICT_RENAME
                    : conflict == "skip"    ? RCS_FILE_CONFLICT_SKIP
                    : conflict == "fail"    ? RCS_FILE_CONFLICT_FAIL
                                            : 0;
        if (!conflict_) {
            throw std::invalid_argument("--file-conflict takes overwrite, rename, skip or fail");
        }
    }
    bool used() const { return !send_.empty() || !publish_.empty() || !receiveDir_.empty() || clipboardFlow_; }
    // --clipboard-files: files copied go (SEND) and come (RECEIVE) as the clipboard's do.
    void carryClipboard(uint32_t flow) {
        clipboardFlow_ = ((flow & RCS_CLIPBOARD_SEND) ? uint32_t(RCS_FILE_SEND) : 0u) |
                         ((flow & RCS_CLIPBOARD_RECEIVE) ? uint32_t(RCS_FILE_RECEIVE) : 0u);
    }
    // --clipboard-host with --clipboard-files: the files of a copy that came
    // are fetched into --receive-dir (by poll()), when it is given.
    void clipboardFiles(rcs_viewer_id viewer, const uint8_t* publication) {
        std::lock_guard lock(mutex_);
        if (!receiveDir_.empty()) {
            copied_ = {viewer, std::vector<uint8_t>(publication, publication + RCS_FILE_PUBLICATION_SIZE)};
        }
    }
    rcs_file_transfer_settings settings() const {
        rcs_file_transfer_settings settings = RCS_FILE_TRANSFER_SETTINGS_INIT;
        settings.flow = (send_.empty() && publish_.empty() ? 0u : RCS_FILE_SEND) |
                        (receiveDir_.empty() ? 0u : RCS_FILE_RECEIVE) | clipboardFlow_;
        settings.max_items = 100000; // Large trees fetched or sent at once.
        settings.max_pending_items = settings.max_pending_items_total = 1000000;
        return settings;
    }
    void attach(rcs_session session) { session_ = session; }

    // One of its events (they never overlap).
    void event(rcs_viewer_id viewer, const rcs_module_event& m, const std::string& who) {
        if (m.kind == RCS_FILE_PEER && m.size == sizeof(rcs_file_peer)) {
            const auto& p = *static_cast<const rcs_file_peer*>(m.payload);
            std::cout << who << "files: the other end " << ((p.flow & RCS_FILE_RECEIVE) ? "receives" : "does not receive")
                      << " and " << ((p.flow & RCS_FILE_SEND) ? "sends" : "does not send") << std::endl;
            if ((p.flow & RCS_FILE_RECEIVE) && !send_.empty() && sentTo_.insert(viewer).second) {
                offer(viewer, who);
            }
            if ((p.flow & RCS_FILE_RECEIVE) && !publish_.empty() && publishedTo_.insert(viewer).second) {
                publish(viewer, who);
            }
            if ((p.flow & RCS_FILE_SEND) && (!fetch_.empty() || !fetchRead_.empty())) {
                {
                    std::lock_guard lock(mutex_);
                    sender_ = viewer;
                }
                poll();
            }
        } else if (m.kind == RCS_FILE_OFFER && m.size == sizeof(rcs_file_offer_event)) {
            const auto& o = *static_cast<const rcs_file_offer_event*>(m.payload);
            std::cout << who << "file transfer " << o.transfer << " offered: " << o.items << " items, " << o.total_bytes
                      << " bytes" << (o.resuming ? " (again, to go on)" : "") << std::endl;
            const rcs_result accepted = rcs_file_accept(session_, o.transfer, receiveDir_.c_str(), conflict_);
            std::cout << who << "file transfer " << o.transfer << " accepted into " << receiveDir_ << ": "
                      << rcs_result_message(accepted)
                      << (accepted == RCS_OK ? std::string() : " (" + rcsdk::lastCallError() + ")") << std::endl;
        } else if (m.kind == RCS_FILE_PROGRESS && m.size == sizeof(rcs_file_progress_event)) {
            const auto& p = *static_cast<const rcs_file_progress_event*>(m.payload);
            const auto now = std::chrono::steady_clock::now();
            auto& last = printed_[p.transfer];
            if (now - last >= 1s) {
                last = now;
                std::cout << who << "file transfer " << p.transfer << ": " << p.done_bytes << " of " << p.total_bytes
                          << " bytes, " << p.bytes_per_second / 1000 << " kB/s" << std::endl;
            }
        } else if (m.kind == RCS_FILE_ITEM_DONE && m.size == sizeof(rcs_file_item_event)) {
            const auto& e = *static_cast<const rcs_file_item_event*>(m.payload);
            const char* states[] = {"?", "pending", "going", "completed", "skipped", "failed"};
            rcs_file_item_info item{};
            uint32_t count = 0;
            rcs_file_get_items(session_, e.transfer, e.item, &item, sizeof(item), 1, &count);
            std::cout << who << "file transfer " << e.transfer << " item " << e.item << " (" << item.path
                      << "): " << states[e.state <= RCS_FILE_ITEM_FAILED ? e.state : 0]
                      << (item.saved_as[0] && std::strcmp(item.saved_as, item.path) ? std::string(" as ") + item.saved_as
                                                                                    : std::string())
                      << (e.message[0] ? std::string(" (") + e.message + ")" : std::string()) << std::endl;
        } else if (m.kind == RCS_FILE_DONE && m.size == sizeof(rcs_file_done_event)) {
            const auto& d = *static_cast<const rcs_file_done_event*>(m.payload);
            const char* ends[] = {"?", "completed", "rejected", "cancelled", "failed"};
            std::cout << who << "file transfer " << d.transfer << (d.direction == RCS_FILE_SEND ? " (sent) " : " (received) ")
                      << ends[d.end <= RCS_FILE_FAILED ? d.end : 0] << (d.by_peer ? " by the other end" : "")
                      << (d.result == RCS_OK ? std::string() : std::string(": ") + rcs_result_message(d.result))
                      << (d.message[0] ? std::string(" (") + d.message + ")" : std::string()) << "; "
                      << d.items_completed << " items completed, " << d.items_skipped << " skipped, "
                      << d.items_failed << " failed, " << d.done_bytes << " bytes"
                      << (d.partial_kept ? "; partial files kept for another run" : "") << std::endl;
        } else if (m.kind == RCS_FILE_STATE && m.size == sizeof(rcs_file_state_event)) {
            const auto& e = *static_cast<const rcs_file_state_event*>(m.payload);
            const char* states[] = {"?", "preparing", "offered", "running", "paused", "waiting", "finished"};
            std::cout << who << "file transfer " << e.transfer << ": " << states[e.state <= RCS_FILE_FINISHED ? e.state : 0]
                      << std::endl;
        }
    }

private:
    void offer(rcs_viewer_id viewer, const std::string& who) {
        std::vector<rcs_file_item> items(send_.size());
        for (size_t i = 0; i < send_.size(); ++i) {
            items[i].path = send_[i].c_str();
        }
        rcs_file_offer offer = RCS_FILE_OFFER_INIT;
        offer.viewer_id = viewer;
        offer.count = uint32_t(items.size());
        offer.items = items.data();
        uint64_t transfer = 0;
        const rcs_result sent = rcs_file_send(session_, &offer, &transfer);
        std::cout << who << "file transfer " << transfer << " of " << send_.size() << " path(s): "
                  << rcs_result_message(sent) << (sent == RCS_OK ? std::string() : " (" + rcsdk::lastCallError() + ")")
                  << std::endl;
    }
    void publish(rcs_viewer_id viewer, const std::string& who) {
        std::vector<rcs_file_item> items(publish_.size());
        for (size_t i = 0; i < publish_.size(); ++i) {
            items[i].path = publish_[i].c_str();
        }
        rcs_file_publication publication = RCS_FILE_PUBLICATION_INIT;
        publication.viewer_id = viewer;
        publication.count = uint32_t(items.size());
        publication.items = items.data();
        publication.on_fetch = [](const uint8_t* id, uint64_t transfer, void*) {
            std::cout << "publication " << publicationHex(id) << " fetched: transfer " << transfer << std::endl;
        };
        uint8_t id[RCS_FILE_PUBLICATION_SIZE] = {};
        const rcs_result published = rcs_file_publish(session_, &publication, id);
        std::cout << who << "published " << publish_.size() << " path(s): " << rcs_result_message(published)
                  << (published == RCS_OK ? "; the other end fetches it with --fetch " + publicationHex(id)
                                          : " (" + rcsdk::lastCallError() + ")")
                  << std::endl;
        if (published == RCS_OK && !publishOut_.empty()) {
            std::ofstream(publishOut_) << publicationHex(id) << "\n";
        }
    }
public:
    // --fetch, --fetch-read: once the other end sends files and the id is
    // known (given, or written to the file given, e.g. by the other end's
    // --publish-out), it is fetched, once.
    void poll() {
        std::unique_lock lock(mutex_);
        if (copied_) {
            const auto [viewer, id] = *copied_;
            copied_.reset();
            lock.unlock();
            rcs_file_fetch_request request = RCS_FILE_FETCH_REQUEST_INIT;
            request.viewer_id = viewer;
            std::copy(id.begin(), id.end(), request.publication);
            request.directory = receiveDir_.c_str();
            request.on_conflict = conflict_;
            uint64_t transfer = 0;
            const rcs_result fetched = rcs_file_fetch(session_, &request, &transfer);
            std::cout << "clipboard files: fetch " << transfer << " into " << receiveDir_ << ": "
                      << rcs_result_message(fetched)
                      << (fetched == RCS_OK ? std::string() : " (" + rcsdk::lastCallError() + ")") << std::endl;
            lock.lock();
        }
        if (!sender_ || fetched_) {
            return;
        }
        const std::string& given = fetch_.empty() ? fetchRead_ : fetch_;
        std::string hex = given;
        if (hex.size() != 2 * RCS_FILE_PUBLICATION_SIZE) {
            std::ifstream file(given);
            hex.clear();
            std::getline(file, hex);
        }
        if (hex.size() != 2 * RCS_FILE_PUBLICATION_SIZE) {
            return;
        }
        fetched_ = true;
        const rcs_viewer_id viewer = *sender_;
        lock.unlock();
        fetch(viewer, publicationOf(hex));
    }

private:
    void fetch(rcs_viewer_id viewer, const std::vector<uint8_t>& id) {
        const bool reading = fetch_.empty();
        rcs_file_fetch_request request = RCS_FILE_FETCH_REQUEST_INIT;
        request.viewer_id = viewer;
        std::copy(id.begin(), id.end(), request.publication);
        request.mode = reading ? RCS_FILE_FETCH_READ : RCS_FILE_FETCH_INTO;
        if (reading) {
            reader_ = std::make_unique<FetchReader>(session_, receiveDir_);
            reader_->into(request);
        } else {
            request.directory = receiveDir_.c_str();
            request.on_conflict = conflict_;
        }
        uint64_t transfer = 0;
        const rcs_result fetched = rcs_file_fetch(session_, &request, &transfer);
        std::cout << "fetch " << transfer << (reading ? " (read)" : " (into " + receiveDir_ + ")") << ": "
                  << rcs_result_message(fetched)
                  << (fetched == RCS_OK ? std::string() : " (" + rcsdk::lastCallError() + ")") << std::endl;
    }

    std::vector<std::string> send_, publish_;
    std::string receiveDir_, fetch_, fetchRead_, publishOut_;
    rcs_file_conflict conflict_ = RCS_FILE_CONFLICT_RENAME;
    rcs_session session_ = nullptr;
    std::set<rcs_viewer_id> sentTo_, publishedTo_;
    std::mutex mutex_; // The events' thread and the program's loop (poll).
    std::optional<rcs_viewer_id> sender_;
    bool fetched_ = false;
    uint32_t clipboardFlow_ = 0;
    std::optional<std::pair<rcs_viewer_id, std::vector<uint8_t>>> copied_;
    std::map<uint64_t, std::chrono::steady_clock::time_point> printed_;
    std::unique_ptr<FetchReader> reader_; // Lives as long as the session (its callbacks may come until it ends).
};

void enableLog(const Arguments& args) {
    if (!args.has("--log")) {
        return;
    }
    rcsdk::setLogCallback(logLevel(args, "--log"), [](const rcs_log_record& record) {
        static const char* levels[] = {"", "error", "warning", "info", "debug"};
        static const char* sources[] = {"process", "session", "agent", "supervisor"};
        std::cerr << "[" << levels[record.level] << "] " << sources[record.source] << " " << record.module << ": "
                  << record.message << std::endl;
    });
}

rcsdk::ConnectionKey readKey(const std::string& path, const char* option) {
    rcsdk::ConnectionKey key{};
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(key.data()), key.size())) {
        throw std::invalid_argument(std::string(option) + " must name a 32-byte key (create one with --generate-key)");
    }
    return key;
}

const char* stateName(rcs_state state) {
    static const char* names[] = {"idle",    "starting",     "listening", "connecting",
                                  "running", "reconnecting", "stopped",   "failed"};
    return state <= RCS_STATE_FAILED ? names[state] : "?";
}

// The agent of a session that runs its capture elsewhere: attached until stopped.
int runAgent(const Arguments& args) {
    enableLog(args);
    if (!args.has("--agent-endpoint")) {
        throw std::invalid_argument("--agent needs --agent-endpoint and --agent-key-file");
    }
    const auto key = readKey(args.get("--agent-key-file"), "--agent-key-file");
    rcsdk::AgentConfig agentConfig(args.get("--agent-endpoint"), key);
    agentConfig.setReconnect(args.get("--reconnect", "on") == "on");
    RawEncoder rawEncoder; // --host-codec: the agent encodes, so the host's encoder is the agent's.
    if (args.has("--host-codec")) {
        agentConfig.setHostEncoder(
            rcsdk::hostEncoder(rawEncoder, {rawCodec}, "rcsdk_cli raw", true, RCS_ACCELERATION_SOFTWARE));
    }
    if (args.has("--session-log")) {
        agentConfig.setLogLevel(logLevel(args, "--session-log"));
    }
    rcsdk::Agent agent(agentConfig, [](const rcs_agent_event& event) {
        std::cout << "agent: " << stateName(event.state.value);
        if (event.state.result != RCS_OK) {
            std::cout << " (" << rcs_result_message(event.state.result) << ")";
        }
        std::cout << std::endl;
    });
    if (const auto result = agent.start(); result != RCS_OK) {
        std::cerr << "Start failed: " << rcs_result_message(result) << "\n";
        return 1;
    }
    const uint32_t seconds = args.number("--seconds", 0);
    const auto started = std::chrono::steady_clock::now();
    while (!interrupted && agent.state() != RCS_STATE_STOPPED && agent.state() != RCS_STATE_FAILED) {
        waitFor(50ms);
        if (seconds && std::chrono::steady_clock::now() - started >= std::chrono::seconds(seconds)) {
            break;
        }
    }
    const bool failed = agent.state() == RCS_STATE_FAILED;
    if (failed) {
        std::cerr << "The agent failed: " << agent.failure().message << "\n";
    }
    agent.stop();
    return failed ? 1 : 0;
}

// Types `text` as key events: letters, digits, space, newline and . - _ @ (USB HID usages, shift where needed).
void typeText(rcsdk::Session& session, const std::string& text) {
    const auto key = [&](uint32_t usage, bool down) {
        rcs_input_event event = RCS_INPUT_EVENT_INIT;
        event.type = RCS_INPUT_KEY;
        event.code = usage;
        event.down = down ? 1u : 0u;
        session.sendInput(event);
    };
    for (const char c : text) {
        uint32_t usage = 0;
        bool shift = false;
        if (c >= 'a' && c <= 'z') {
            usage = 4 + uint32_t(c - 'a');
        } else if (c >= 'A' && c <= 'Z') {
            usage = 4 + uint32_t(c - 'A');
            shift = true;
        } else if (c >= '1' && c <= '9') {
            usage = 30 + uint32_t(c - '1');
        } else if (c == '0') {
            usage = 39;
        } else if (c == ' ') {
            usage = 44;
        } else if (c == '\n') {
            usage = 40;
        } else if (c == '.') {
            usage = 55;
        } else if (c == '-') {
            usage = 45;
        } else if (c == '_') {
            usage = 45;
            shift = true;
        } else if (c == '@') {
            usage = 31;
            shift = true;
        } else {
            throw std::invalid_argument(std::string("--type cannot type '") + c + "'");
        }
        if (shift) {
            key(225, true);
        }
        key(usage, true);
        key(usage, false);
        if (shift) {
            key(225, false);
        }
        std::this_thread::sleep_for(15ms);
    }
}

// Types `text` (UTF-8) as text events, one per character: what any layout or
// input method could produce.
size_t sendText(rcsdk::Session& session, const std::string& text) {
    size_t sent = 0;
    for (size_t i = 0; i < text.size();) {
        const auto byte = uint8_t(text[i]);
        const int length = byte < 0x80 ? 1 : byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 0;
        if (!length || i + length > text.size()) {
            throw std::invalid_argument("--text is not UTF-8");
        }
        uint32_t character = length == 1 ? byte : byte & (0x3F >> (length - 1));
        for (int k = 1; k < length; ++k) {
            character = character << 6 | (uint8_t(text[i + k]) & 0x3F);
        }
        i += size_t(length);
        rcs_input_event event = RCS_INPUT_EVENT_INIT;
        event.type = RCS_INPUT_TEXT;
        event.code = character;
        if (session.sendInput(event) == RCS_OK) {
            ++sent;
        }
        std::this_thread::sleep_for(15ms);
    }
    return sent;
}

// A viewer's number in the signal room, as a host's server would name its users.
uint64_t newViewerId() {
    static std::mt19937_64 random{std::random_device{}()};
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    return random() >> 12 | 1; // Short enough to read, never 0.
}

// A line of the signal room: "view <id> <signal>" is for the sharer, "share
// <id> <signal>" for viewer <id>. False for a line that is someone else's.
bool parseSignalLine(const std::string& line, const char* from, uint64_t& id, std::string& signal) {
    std::istringstream in(line);
    std::string kind;
    return in >> kind >> id >> signal && kind == from;
}

// --viewers N: N viewers in this process, each with its own connection and,
// over P2P and RELAY, its own signals through the signal room. One line a
// second for all of them; --report lists each.
int runViewers(const Arguments& args, const rcsdk::SessionConfig& config, uint32_t count) {
    const std::string transport = args.get("--transport", "udp");
    const bool signalled = transport == "p2p" || transport == "relay";
    if (signalled && !args.has("--signal-relay")) {
        throw std::invalid_argument("--viewers over p2p or relay needs --signal-relay (one signal file serves one)");
    }
    struct Watcher {
        uint32_t index = 0;
        uint64_t id = 0;
        std::unique_ptr<RawDecoder> decoder;        // --host-codec: its own.
        std::unique_ptr<relay::RelayChannel> relay; // HOST: its own channel through rcsdk_host_relay.
        std::unique_ptr<rcsdk::Session> session;
        ~Watcher() {
            if (relay) {
                relay->stop(); // No delivery reaches the session once it goes.
            }
        }
    };
    std::vector<std::unique_ptr<Watcher>> viewers;
    std::unique_ptr<relay::SignalBoard> board; // Made once every viewer is.
    for (uint32_t i = 0; i < count; ++i) {
        auto watcher = std::make_unique<Watcher>();
        watcher->index = i + 1;
        watcher->id = newViewerId();
        auto own = config;
        if (signalled && !args.has("--port")) { // Each viewer's path on a port of its own.
            own.setAddress(args.get("--address", "127.0.0.1"), 0);
        }
        if (args.has("--host-codec")) {
            watcher->decoder = std::make_unique<RawDecoder>();
            own.use(rcsdk::hostDecoder(*watcher->decoder, {rawCodec}, "rcsdk_cli raw", RCS_ACCELERATION_SOFTWARE));
        }
        if (transport == "host") {
            const rcs_host_channel kind =
                args.has("--relay-datagram") ? RCS_HOST_CHANNEL_DATAGRAM : RCS_HOST_CHANNEL_STREAM;
            watcher->relay =
                std::make_unique<relay::RelayChannel>(args.get("--relay"), args.get("--room", "rcsdk"), kind);
            own.use(rcsdk::hostTransport(*watcher->relay, kind, args.number("--host-max-datagram", 0)));
        }
        rcsdk::Callbacks callbacks;
        const std::string name = "[viewer " + std::to_string(i + 1) + "] ";
        callbacks.onEvent = [name](const rcs_event& event) {
            static const char* names[] = {"idle",    "starting",     "listening", "connecting",
                                          "running", "reconnecting", "stopped",   "failed"};
            if (event.type == RCS_EVENT_STATE) {
                const auto& state = event.state;
                std::cout << name << "state: " << (state.value <= RCS_STATE_FAILED ? names[state.value] : "?")
                          << (state.result != RCS_OK ? std::string(" (") + rcs_result_message(state.result) + ")"
                                                     : std::string())
                          << std::endl;
            }
        };
        callbacks.onSignal = [&board, id = watcher->id](const std::string& text, uint64_t) {
            board->send("view " + std::to_string(id) + " " + text);
        };
        watcher->session = std::make_unique<rcsdk::Session>(own, std::move(callbacks));
        if (watcher->relay) {
            watcher->relay->start(*watcher->session);
        }
        viewers.push_back(std::move(watcher));
    }
    if (signalled) {
        board = std::make_unique<relay::SignalBoard>(
            args.get("--signal-relay"), args.get("--room", "rcsdk"), [&viewers](const std::string& line) {
                uint64_t id = 0;
                std::string signal;
                if (!parseSignalLine(line, "share", id, signal)) {
                    return;
                }
                for (const auto& viewer : viewers) {
                    if (viewer->id == id) {
                        const auto result = viewer->session->signal(signal);
                        std::cout << "[viewer " << viewer->index << "] answer: " << rcs_result_message(result)
                                  << std::endl;
                    }
                }
            });
    }
    struct BoardStop { // No line reaches a session once this goes, before they do.
        std::unique_ptr<relay::SignalBoard>& board;
        ~BoardStop() { board.reset(); }
    } boardStop{board};
    const auto interval = std::chrono::milliseconds(args.number("--viewer-interval", 20));
    for (const auto& viewer : viewers) {
        std::cout << "[viewer " << viewer->index << "] id " << viewer->id << std::endl;
        if (viewer->index > 1) {
            std::this_thread::sleep_for(interval); // People join one after another, not in the same instant.
        }
        if (const auto result = viewer->session->start(); result != RCS_OK) {
            std::cerr << "[viewer " << viewer->index << "] start failed: " << rcs_result_message(result) << ": "
                      << viewer->session->failure().message << "\n";
        }
    }
    const uint32_t seconds = args.number("--seconds", 0);
    const auto started = std::chrono::steady_clock::now();
    auto nextReport = started + 1s;
    uint64_t lastFrames = 0;
    const auto frames = [&] {
        uint64_t total = 0;
        for (const auto& viewer : viewers) {
            total += viewer->session->stats<rcs_video_receive_stats>().decoded_frames;
        }
        return total;
    };
    while (!interrupted) {
        waitFor(50ms);
        const auto now = std::chrono::steady_clock::now();
        size_t running = 0, ended = 0;
        for (const auto& viewer : viewers) {
            const rcs_state state = viewer->session->state();
            running += state == RCS_STATE_RUNNING;
            ended += state == RCS_STATE_FAILED || state == RCS_STATE_STOPPED;
        }
        if (ended == viewers.size() || (seconds && now - started >= std::chrono::seconds(seconds))) {
            break;
        }
        if (now >= nextReport) {
            const uint64_t total = frames();
            std::cout << "viewers: " << running << " of " << viewers.size() << " running, " << ended
                      << " ended, decoded " << total - lastFrames << " frames/s in all" << std::endl;
            lastFrames = total;
            nextReport = now + 1s;
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::ostringstream report;
    report << "{\n  \"version\": \"" << rcs_version() << "\",\n  \"seconds\": " << elapsed << ",\n  \"viewers\": [";
    bool failed = false;
    for (const auto& viewer : viewers) {
        const auto& session = *viewer->session;
        const auto failure = session.failure();
        const auto network = session.stats<rcs_network_stats>();
        const uint64_t decoded = session.stats<rcs_video_receive_stats>().decoded_frames;
        failed = failed || session.state() == RCS_STATE_FAILED;
        if (session.state() == RCS_STATE_FAILED) {
            std::cerr << "[viewer " << viewer->index << "] failed (" << rcs_result_message(failure.result)
                      << "): " << failure.message << "\n";
        }
        report << (viewer->index > 1 ? "," : "") << "\n    {\"index\": " << viewer->index << ", \"id\": " << viewer->id
               << ", \"state\": " << session.state() << ", \"result\": " << failure.result << ", \"error\": \""
               << failure.message << "\", \"frames\": " << decoded
               << ", \"fps\": " << (elapsed > 0 ? decoded / elapsed : 0) << ", \"path\": \"" << network.path
               << "\", \"round_trip_ms\": " << network.round_trip_ns / 1e6 << ", \"bytes\": " << network.bytes
               << ", \"connections\": " << network.connections << "}";
    }
    report << "\n  ]\n}\n";
    for (const auto& viewer : viewers) {
        viewer->session->stop();
    }
    if (args.has("--report")) {
        std::ofstream(args.get("--report")) << report.str();
    }
    return failed ? 1 : 0;
}

int run(const Arguments& args) {
    const std::string role = args.get("--role");
    if (role != "share" && role != "view") {
        usage();
        return 2;
    }
    enableLog(args);
    const bool share = role == "share";
    rcsdk::SessionConfig config(share ? RCS_ROLE_SHARE : RCS_ROLE_VIEW, presetOf(args));
    config.setAddress(args.get("--address", "127.0.0.1"), args.number("--port", 47990));
    if (args.has("--session-log")) {
        config.setLogLevel(logLevel(args, "--session-log"));
    }
    // Identity: the connection key (default), a password, a PIN, or device keys alone.
    const std::string secret = args.has("--password") ? "password"
                               : args.has("--pin")    ? "pin"
                                                      : args.get("--secret", "key");
    if (secret == "key") {
        std::ifstream keyFile(args.get("--key-file"), std::ios::binary);
        if (!keyFile.read(reinterpret_cast<char*>(config.core().identity.key), sizeof(config.core().identity.key))) {
            std::cerr << "--key-file must name a 32-byte key (create one with --generate-key)\n";
            return 2;
        }
    } else if (secret == "password") {
        config.setPassword(args.get("--password"));
    } else if (secret == "pin") {
        config.setPin(args.get("--pin"), args.number("--pin-attempts", 5));
    } else if (secret == "none") {
        config.setSecret(RCS_SECRET_NONE);
    } else {
        throw std::invalid_argument("--secret takes key or none (or use --password or --pin)");
    }
    if (args.has("--device-key")) {
        rcsdk::DeviceKey key{};
        std::ifstream file(args.get("--device-key"), std::ios::binary);
        if (!file.read(reinterpret_cast<char*>(key.data()), key.size())) {
            throw std::invalid_argument("--device-key must name a device key file (see --generate-device-key)");
        }
        config.setDeviceKey(key);
    }
    if (args.has("--trust")) {
        std::vector<rcsdk::PublicKey> trusted;
        std::stringstream list(args.get("--trust"));
        for (std::string text; std::getline(list, text, ',');) {
            trusted.push_back(publicKeyFromHex(text));
        }
        config.setTrustedKeys(std::move(trusted));
    }
    // Only what the arguments set is configured; the preset covers the rest. An
    // extension touched starts from the preset's settings (SessionConfig::extension).
    std::unique_ptr<RateCeiling> rateCeiling; // Outlives the session.
    if (share) {
        if (args.has("--width") || args.has("--height") || args.has("--fps") || args.has("--bitrate")) {
            auto& video = config.shareVideo();
            video.width = args.number("--width", video.width);
            video.height = args.number("--height", video.height);
            video.frames_per_second = args.number("--fps", video.frames_per_second);
            video.bitrate_bps = args.number("--bitrate", video.bitrate_bps);
        }
        config.capture() = captureOf(args);
        if (args.has("--host-pattern")) {
            config.setCaptureSource(RCS_CAPTURE_HOST);
        }
        if (args.has("--input-boost-ms")) {
            config.extension<rcs_video_tuning_config>().input_boost_ms = args.number("--input-boost-ms", 0);
        }
        if (args.has("--keyframe-interval")) {
            config.extension<rcs_video_tuning_config>().keyframe_interval_ms = args.number("--keyframe-interval", 0);
        }
        if (args.has("--rate-rules")) {
            uint32_t overuse = 0, severe = 0, clear = 0;
            if (std::sscanf(args.get("--rate-rules").c_str(), "%u,%u,%u", &overuse, &severe, &clear) != 3) {
                throw std::invalid_argument("--rate-rules takes three numbers, e.g. 100,200,60");
            }
            auto& rules = config.extension<rcs_video_tuning_config>();
            rules.overuse_queue_ms = overuse;
            rules.severe_queue_ms = severe;
            rules.clear_queue_ms = clear;
        }
        if (args.has("--pacing")) {
            uint32_t spread = 0, mbps = 0;
            if (std::sscanf(args.get("--pacing").c_str(), "%u,%u", &spread, &mbps) != 2) {
                throw std::invalid_argument("--pacing takes two numbers, e.g. 1000,800");
            }
            auto& rules = config.extension<rcs_video_tuning_config>();
            rules.frame_spread_us = spread;
            rules.max_pacing_bps = mbps * 1'000'000;
        }
        if (args.has("--path-pacing")) {
            uint32_t percent = 0, burst = 0;
            if (std::sscanf(args.get("--path-pacing").c_str(), "%u,%u", &percent, &burst) != 2) {
                throw std::invalid_argument("--path-pacing takes two numbers, e.g. 110,8000");
            }
            auto& rules = config.extension<rcs_video_tuning_config>();
            rules.path_pacing_percent = percent;
            rules.path_burst_us = burst;
        }
        if (args.has("--fec")) {
            const std::string fec = args.get("--fec");
            auto& rules = config.extension<rcs_video_tuning_config>();
            const auto comma = fec.find(',');
            const std::string mode = fec.substr(0, comma);
            rules.fec_mode = mode == "off" ? RCS_FEC_OFF : mode == "fixed" ? RCS_FEC_FIXED : RCS_FEC_AUTO;
            if (mode != "off" && mode != "auto" && mode != "fixed") {
                throw std::invalid_argument("--fec takes off, auto or fixed, and a percent, e.g. fixed,20");
            }
            if (comma != std::string::npos) {
                rules.fec_percent = uint32_t(std::stoul(fec.substr(comma + 1)));
            }
        }
        if (args.has("--rate-cap") || args.has("--rate-trace")) {
            rateCeiling = std::make_unique<RateCeiling>(args.number("--rate-cap", 0) * 1000, args.get("--rate-trace"));
            config.use(rcsdk::rateDecider(*rateCeiling));
        }
        if (args.has("--allow-control")) {
            config.setControl(true, args.has("--host-input") ? RCS_INPUT_HOST : RCS_INPUT_SYSTEM);
            const std::string capsLock = args.get("--caps-lock", "input-source");
            if (capsLock != "input-source" && capsLock != "caps-lock") {
                throw std::invalid_argument("--caps-lock takes input-source or caps-lock");
            }
            config.setCapsLock(capsLock == "caps-lock" ? RCS_CAPS_LOCK_CAPS_LOCK : RCS_CAPS_LOCK_INPUT_SOURCE);
        }
        if (args.has("--max-viewers") || args.has("--bitrate-from")) {
            const std::string from = args.get("--bitrate-from", "quorum");
            if (from != "quorum" && from != "min" && from != "main") {
                throw std::invalid_argument("--bitrate-from takes quorum, min or main");
            }
            config.setViewers(args.number("--max-viewers", 0), from == "main"  ? RCS_BITRATE_FROM_MAIN
                                                               : from == "min" ? RCS_BITRATE_FROM_MINIMUM
                                                                               : RCS_BITRATE_FROM_QUORUM);
        }
        if (args.has("--encodings")) {
            const std::string encodings = args.get("--encodings", "auto");
            auto& e = config.encodings();
            if (encodings == "single") {
                e.mode = RCS_ENCODINGS_SINGLE;
            } else if (encodings != "auto") {
                e.mode = RCS_ENCODINGS_FIXED;
                e.count = args.number("--encodings", 2);
            }
        }
        if (args.has("--layers")) {
            auto& e = config.encodings();
            e.scalability = args.number("--layers", 1);
            for (auto& lower : e.lower) {
                lower.scalability = e.scalability;
            }
        }
        if (args.has("--secure-desktop")) {
            config.setSecureDesktop(true);
        }
        if (args.has("--agent-endpoint")) {
            const std::string access = args.get("--agent-access", "privileged");
            if (access != "privileged" && access != "any") {
                throw std::invalid_argument("--agent-access takes privileged or any");
            }
            config.use(rcsdk::agentLink(args.get("--agent-endpoint"),
                                        readKey(args.get("--agent-key-file"), "--agent-key-file"),
                                        access == "any" ? RCS_AGENT_ACCESS_ANY_USER : RCS_AGENT_ACCESS_PRIVILEGED));
        }
        if (args.has("--color")) {
            const std::string color = args.get("--color");
            const auto comma = color.find(',');
            const std::string matrix = color.substr(0, comma);
            const std::string range = comma == std::string::npos ? "" : color.substr(comma + 1);
            if ((matrix != "bt709" && matrix != "bt601") || (range != "limited" && range != "full")) {
                throw std::invalid_argument("--color takes bt709|bt601,limited|full");
            }
            config.setColor(matrix == "bt601" ? RCS_MATRIX_BT601 : RCS_MATRIX_BT709,
                            range == "full" ? RCS_RANGE_FULL : RCS_RANGE_LIMITED);
        }
    } else if (args.has("--width") || args.has("--fps") || args.has("--bitrate") || args.has("--request-format")) {
        // The viewer asks for the stream; the sharer follows within its limits.
        auto& request = config.viewRequest();
        request.width = args.number("--width", request.width);
        request.height = args.number("--height", request.height);
        request.frames_per_second = args.number("--fps", request.frames_per_second);
        request.bitrate_bps = args.number("--bitrate", request.bitrate_bps);
        if (args.has("--request-format")) {
            request.format = parseFormat(args.get("--request-format"));
        }
    }
    if (args.has("--text-caret")) {
        config.useModule(RCS_TEXT_CARET_MODULE, nullptr, 0);
    }
    if (args.has("--keep-awake") || args.has("--power-actions") || args.has("--share-wake-info") ||
        args.has("--power-ask")) {
        rcs_power_settings power = RCS_POWER_SETTINGS_INIT;
        power.keep_awake = RCS_KEEP_AWAKE_OFF; // Unless asked: the other options are not about staying awake.
        if (args.has("--keep-awake")) {
            const std::string awake = args.get("--keep-awake");
            power.keep_awake = awake.rfind("display", 0) == 0 ? RCS_KEEP_AWAKE_DISPLAY : RCS_KEEP_AWAKE_SYSTEM;
            power.while_running = awake.find(",run") != std::string::npos ? 1 : 0;
        }
        if (share && args.has("--power-actions")) {
            const std::string list = args.get("--power-actions");
            if (list == "all") {
                power.actions = RCS_POWER_ALL_ACTIONS;
            } else {
                std::stringstream names(list);
                for (std::string name; std::getline(names, name, ',');) {
                    power.actions |= 1u << powerActionNamed(name);
                }
            }
            power.action_delay_ms = args.number("--power-delay", power.action_delay_ms);
        }
        power.share_wake_info = share && args.has("--share-wake-info") ? 1 : 0;
        config.useModule(RCS_POWER_MODULE, power);
    }
    if (share && args.has("--session-thumbnail")) {
        config.useModule(RCS_THUMBNAIL_MODULE, nullptr, 0);
    }
    FileHost files(args);
    if (args.has("--clipboard")) {
        const std::string flow = args.get("--clipboard");
        if (flow != "send" && flow != "receive" && flow != "both") {
            throw std::invalid_argument("--clipboard takes send, receive or both");
        }
        rcs_clipboard_settings clipboard = RCS_CLIPBOARD_SETTINGS_INIT;
        clipboard.flow = flow == "send"      ? RCS_CLIPBOARD_SEND
                         : flow == "receive" ? RCS_CLIPBOARD_RECEIVE
                                             : RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
        clipboard.send_concealed = args.has("--clipboard-concealed") ? 1 : 0;
        clipboard.send_existing = args.has("--clipboard-existing") ? 1 : 0;
        if (args.has("--clipboard-files")) { // Files copied go too, by file transfer as they are pasted.
            clipboard.formats = RCS_CLIPBOARD_ALL_FORMATS | (1u << RCS_CLIPBOARD_FILES);
            files.carryClipboard(clipboard.flow);
            char reason[256] = {};
            const uint32_t carried = rcs_clipboard_system_files(reason, sizeof(reason));
            std::cout << "clipboard files here: " << ((carried & RCS_CLIPBOARD_SEND) ? "read" : "not read") << ", "
                      << ((carried & RCS_CLIPBOARD_RECEIVE) ? "pasted" : "not pasted")
                      << (reason[0] ? std::string(" (") + reason + ")" : std::string()) << std::endl;
        }
        if (args.has("--clipboard-host")) { // Copies stay in this program: printed as they arrive.
            clipboard.target = RCS_CLIPBOARD_HOST;
            clipboard.user_data = &files;
            clipboard.on_clipboard = [](const rcs_clipboard_content* content, void* host) -> int32_t {
                std::ostringstream line;
                line << "clipboard arrived:";
                for (uint32_t i = 0; i < content->count; ++i) {
                    const auto& item = content->items[i];
                    if (item.format == RCS_CLIPBOARD_TEXT) {
                        line << " text \"" << std::string(reinterpret_cast<const char*>(item.data), item.size) << "\"";
                    } else {
                        line << " format " << item.format << " " << item.size << " bytes";
                    }
                }
                if (const auto* f = content->files) {
                    line << " files" << (f->cut ? " (cut)" : "") << ":";
                    for (uint32_t i = 0; i < f->count; ++i) {
                        line << " \"" << f->files[i].name << "\""
                             << (f->files[i].kind == RCS_FILE_DIRECTORY ? " (directory)"
                                                                        : " " + std::to_string(f->files[i].size) +
                                                                              " bytes");
                    }
                    line << ", publication " << publicationHex(f->publication);
                    static_cast<FileHost*>(host)->clipboardFiles(content->viewer_id, f->publication);
                }
                std::cout << line.str() << std::endl;
                return 1;
            };
        }
        config.useModule(RCS_CLIPBOARD_MODULE, clipboard);
    }
    if (args.has("--pointer")) {
        config.useModule(EXAMPLE_POINTER_MODULE, nullptr, 0);
    }
    if (files.used()) {
        config.useModule(RCS_FILE_TRANSFER_MODULE, files.settings());
    }
    if (args.has("--acceleration")) {
        const std::string acceleration = args.get("--acceleration");
        if (acceleration != "hardware" && acceleration != "software" && acceleration != "hardware-first") {
            throw std::invalid_argument("--acceleration takes hardware, software or hardware-first");
        }
        config.setAcceleration(acceleration == "software"         ? RCS_ACCELERATION_SOFTWARE
                               : acceleration == "hardware-first" ? RCS_ACCELERATION_HARDWARE_FIRST
                                                                  : RCS_ACCELERATION_HARDWARE);
    }
    if (args.has("--backend")) {
        const std::string b = args.get("--backend");
        const rcs_backend backend = b == "auto"         ? RCS_BACKEND_AUTO
                                    : b == "mf"         ? RCS_BACKEND_MEDIA_FOUNDATION
                                    : b == "nvenc"      ? RCS_BACKEND_NVENC
                                    : b == "nvdec"      ? RCS_BACKEND_NVDEC
                                    : b == "onevpl"     ? RCS_BACKEND_ONEVPL
                                    : b == "vt"         ? RCS_BACKEND_VIDEOTOOLBOX
                                    : b == "mediacodec" ? RCS_BACKEND_MEDIACODEC
                                    : b == "openh264"   ? RCS_BACKEND_OPENH264
                                    : b == "libvpx"     ? RCS_BACKEND_LIBVPX
                                    : b == "aom"        ? RCS_BACKEND_LIBAOM
                                    : b == "dav1d"      ? RCS_BACKEND_DAV1D
                                                        : rcs_backend(99u);
        if (backend == 99u) {
            throw std::invalid_argument(
                "--backend takes auto, mf, nvenc, nvdec, onevpl, vt, mediacodec, openh264, libvpx, aom or dav1d");
        }
        config.setBackend(backend);
    }
    if (share && args.has("--encoder-speed")) {
        const std::string speed = args.get("--encoder-speed");
        if (speed != "fastest" && speed != "balanced" && speed != "quality") {
            throw std::invalid_argument("--encoder-speed takes fastest, balanced or quality");
        }
        config.extension<rcs_video_tuning_config>().speed = speed == "fastest"    ? RCS_ENCODER_FASTEST
                                                            : speed == "balanced" ? RCS_ENCODER_BALANCED
                                                                                  : RCS_ENCODER_BEST_QUALITY;
    }
    if (share && args.has("--content")) {
        const std::string content = args.get("--content");
        if (content != "auto" && content != "detail" && content != "motion") {
            throw std::invalid_argument("--content takes auto, detail or motion");
        }
        config.extension<rcs_video_tuning_config>().content = content == "detail"   ? RCS_CONTENT_DETAIL
                                                              : content == "motion" ? RCS_CONTENT_MOTION
                                                                                    : RCS_CONTENT_AUTO;
    }
    if (args.has("--software-threads")) {
        config.extension<rcs_video_tuning_config>().software_threads = args.number("--software-threads", 0);
    }
    if (share && args.has("--degradation")) {
        const std::string way = args.get("--degradation");
        if (way != "frame-rate" && way != "size" && way != "balanced") {
            throw std::invalid_argument("--degradation takes frame-rate, size or balanced");
        }
        config.extension<rcs_video_tuning_config>().degradation = way == "frame-rate" ? RCS_DEGRADE_FRAME_RATE
                                                                  : way == "size"       ? RCS_DEGRADE_SIZE
                                                                                        : RCS_DEGRADE_BALANCED;
    }
    RawEncoder rawEncoder;
    RawDecoder rawDecoder;
    if (args.has("--encoded-pattern")) {
        config.setCaptureSource(RCS_CAPTURE_ENCODED).setCodecs({rawCodec});
    } else if (args.has("--host-codec")) {
        config.setCodecs({rawCodec, RCS_CODEC_H264});
        if (share && !args.has("--agent-endpoint")) { // An agent has its own (--agent --host-codec).
            config.use(rcsdk::hostEncoder(rawEncoder, {rawCodec}, "rcsdk_cli raw", true, RCS_ACCELERATION_SOFTWARE));
        } else if (!share) {
            config.use(rcsdk::hostDecoder(rawDecoder, {rawCodec}, "rcsdk_cli raw", RCS_ACCELERATION_SOFTWARE));
        }
    }
    if (args.has("--codec")) {
        parseCodecs(args.get("--codec"), share ? config.encoder().codecs : config.decoder().codecs);
    }
    if (args.has("--formats")) {
        parseFormats(args.get("--formats"), share ? config.encoder().formats : config.decoder().formats);
    }
    if (args.has("--streams")) {
        auto& streams = config.streams();
        size_t i = 0;
        std::stringstream list(args.get("--streams"));
        for (std::string id; std::getline(list, id, ',');) {
            if (i == std::size(streams.displays)) {
                throw std::invalid_argument("--streams takes at most 3 displays");
            }
            streams.displays[i++] = uint32_t(std::stoul(id));
        }
    }
    if (args.has("--receive-streams")) {
        config.setReceiveStreams(args.number("--receive-streams", 0));
    }
    if (args.has("--audio")) {
        const std::string audio = args.get("--audio");
        if (audio != "on" && audio != "off") {
            throw std::invalid_argument("--audio takes on or off");
        }
        config.setAudioEnabled(audio == "on");
    }
    if (args.has("--audio-codec")) {
        parseAudioCodecs(args.get("--audio-codec"), share ? config.sendAudio().codecs : config.receiveAudio().codecs);
    }
    if (args.has("--audio-bitrate")) {
        config.setAudioBitrate(args.number("--audio-bitrate", 128000));
    }
    if (args.has("--exclude-own-sound")) {
        config.setExcludeOwnSound(true);
    }
    if (args.has("--audio-capture")) {
        const std::string b = args.get("--audio-capture");
        const rcs_audio_capture_backend backend = b == "sck"      ? RCS_AUDIO_CAPTURE_SCREENCAPTUREKIT
                                                  : b == "tap"    ? RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP
                                                  : b == "wasapi" ? RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK
                                                  : b == "auto"   ? RCS_AUDIO_CAPTURE_AUTO
                                                                  : 99u;
        if (backend == 99u) {
            throw std::invalid_argument("--audio-capture takes auto, sck, tap or wasapi");
        }
        config.extension<rcs_audio_tuning_config>().capture_backend = backend;
    }
    if (args.has("--capture-period")) {
        config.extension<rcs_audio_tuning_config>().capture_period_frames = args.number("--capture-period", 0);
    }
    if (args.has("--buffer-margin-us")) {
        config.extension<rcs_audio_tuning_config>().buffer_margin_us = args.number("--buffer-margin-us", 2000);
    }
    if (args.has("--playback-period")) {
        config.extension<rcs_audio_tuning_config>().playback_period_frames = args.number("--playback-period", 0);
    }
    if (args.has("--playback-queue")) {
        config.extension<rcs_audio_tuning_config>().playback_queue_frames = args.number("--playback-queue", 0);
    }
    if (args.has("--audio-delay")) {
        config.extension<rcs_audio_tuning_config>().max_delay_ms = args.number("--audio-delay", 200);
    }
    if (args.has("--no-playback")) {
        config.setAudioPlayback(false);
    }
    if (args.has("--send-audio")) {
        const std::string what = args.get("--send-audio");
        auto& send = config.sendAudio();
        const auto comma = what.find(',');
        const std::string source = what.substr(0, comma);
        if (source != "microphone" && source != "host" && source != "off") {
            throw std::invalid_argument("--send-audio takes microphone[,DEVICE], host or off");
        }
        send.enabled = source != "off";
        send.source = source == "microphone" ? RCS_AUDIO_MICROPHONE : RCS_AUDIO_HOST;
        send.device = comma != std::string::npos ? uint32_t(std::stoul(what.substr(comma + 1))) : 0;
        send.voice_processing = args.has("--no-voice-processing") ? 0 : 1;
    }
    if (args.has("--receive-audio")) {
        const std::string what = args.get("--receive-audio");
        if (what != "on" && what != "host" && what != "off") {
            throw std::invalid_argument("--receive-audio takes on, host or off");
        }
        auto& receive = config.receiveAudio();
        receive.enabled = what != "off";
        receive.playback = what == "on";
    }
    if (args.has("--transport")) {
        const std::string transport = args.get("--transport");
        if (transport != "tcp" && transport != "udp" && transport != "p2p" && transport != "relay" &&
            transport != "host" && transport != "forwarder") {
            throw std::invalid_argument("--transport takes tcp, udp, p2p, relay, host or forwarder");
        }
        config.setTransport(transport == "host"        ? RCS_TRANSPORT_HOST
                         : transport == "forwarder" ? RCS_TRANSPORT_FORWARDER
                         : transport == "relay"     ? RCS_TRANSPORT_RELAY
                         : transport == "p2p"       ? RCS_TRANSPORT_P2P
                         : transport == "udp"       ? RCS_TRANSPORT_UDP
                                                    : RCS_TRANSPORT_TCP);
        if (transport == "forwarder") {
            const std::string hex = args.get("--forwarder-key");
            std::array<uint8_t, 32> sha256{};
            if (hex.size() != 2 * sha256.size() || hex.find_first_not_of("0123456789abcdefABCDEF") != hex.npos ||
                !args.has("--token")) {
                throw std::invalid_argument("--transport forwarder needs --forwarder-key (64 hex digits) and --token");
            }
            for (size_t i = 0; i < sha256.size(); ++i) {
                sha256[i] = uint8_t(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
            }
            const std::string link = args.get("--forwarder-link", "udp");
            if (link.rfind("ws://", 0) == 0 || link.rfind("wss://", 0) == 0) {
                std::array<uint8_t, 32> pin{};
                if (args.has("--forwarder-pin")) {
                    const std::string pinHex = args.get("--forwarder-pin");
                    if (pinHex.size() != 64 || pinHex.find_first_not_of("0123456789abcdefABCDEF") != pinHex.npos) {
                        throw std::invalid_argument("--forwarder-pin: 64 hex digits");
                    }
                    for (size_t i = 0; i < pin.size(); ++i) {
                        pin[i] = uint8_t(std::stoul(pinHex.substr(2 * i, 2), nullptr, 16));
                    }
                }
                config.setForwarderWebSocket(link, sha256, args.get("--token"), pin);
            } else if (link == "udp" || link == "tcp") {
                config.setForwarder(args.get("--address", "127.0.0.1"), args.number("--port", 47995), sha256,
                                    args.get("--token"), link == "tcp" ? RCS_FORWARDER_TCP : RCS_FORWARDER_UDP);
            } else {
                throw std::invalid_argument("--forwarder-link takes udp, tcp, or a ws:// or wss:// URL");
            }
        }
        if (transport == "relay") {
            const std::string via = args.get("--turn-transport", "udp");
            if (via != "udp" && via != "tcp" && via != "tls") {
                throw std::invalid_argument("--turn-transport takes udp, tcp or tls");
            }
            config.setRelay(args.get("--turn"), args.get("--turn-user"), args.get("--turn-pass"),
                            via == "tls"   ? RCS_TURN_TLS
                            : via == "tcp" ? RCS_TURN_TCP
                                           : RCS_TURN_UDP);
            if (args.has("--turn-pin")) {
                const std::string hex = args.get("--turn-pin");
                rcsdk::CertificateSha256 pin{};
                if (hex.size() != 2 * pin.size() || hex.find_first_not_of("0123456789abcdefABCDEF") != hex.npos) {
                    throw std::invalid_argument("--turn-pin takes the SHA-256 as 64 hex digits");
                }
                for (size_t i = 0; i < pin.size(); ++i) {
                    pin[i] = uint8_t(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
                }
                config.setRelayCertificate(pin);
            }
            if (args.has("--turn-name")) {
                config.setRelayServerName(args.get("--turn-name"));
            }
        }
        if (transport == "host" && !args.has("--relay")) {
            throw std::invalid_argument("--transport host needs --relay ADDR:PORT (rcsdk_host_relay)");
        }
    }
    if (args.has("--interface")) {
        const std::string name = args.get("--interface");
        auto& network = config.extension<rcs_network_tuning_config>();
        if (name.size() >= sizeof(network.local_interface)) {
            throw std::invalid_argument("--interface takes a name of at most 63 bytes");
        }
        std::memcpy(network.local_interface, name.c_str(), name.size() + 1);
    }
    if (args.has("--audio-dscp")) {
        config.extension<rcs_network_tuning_config>().audio_dscp = args.number("--audio-dscp", 0);
    }
    for (const char* option : {"--keep-listening", "--reconnect"}) {
        if (args.has(option) && args.get(option) != "on" && args.get(option) != "off") {
            throw std::invalid_argument(std::string(option) + " takes on or off");
        }
    }
    if (args.has("--keep-listening")) {
        config.setKeepListening(args.get("--keep-listening") == "on");
    }
    if (args.has("--reconnect")) {
        config.setReconnect(args.get("--reconnect") == "on");
    }
    if (args.has("--reconnect-window")) {
        config.extension<rcs_network_tuning_config>().reconnect_window_ms =
            args.number("--reconnect-window", 30) * 1000;
    }
    if (args.has("--migrate") || args.has("--exclude-networks") || args.has("--path-policy") || args.has("--standby")) {
        auto& paths = config.extension<rcs_paths_config>();
        if (args.has("--migrate")) {
            if (args.get("--migrate") != "on" && args.get("--migrate") != "off") {
                throw std::invalid_argument("--migrate takes on or off");
            }
            paths.migrate = args.get("--migrate") == "on";
        }
        std::stringstream kinds(args.get("--exclude-networks"));
        for (std::string kind; std::getline(kinds, kind, ',');) {
            static const char* const names[] = {"unknown", "ethernet", "wifi", "cellular", "other"};
            const auto named = std::find(std::begin(names), std::end(names), kind);
            if (named == std::end(names)) {
                throw std::invalid_argument("--exclude-networks takes ethernet, wifi, cellular, other or unknown");
            }
            paths.excluded_networks |= 1u << (named - std::begin(names));
        }
        if (args.has("--path-policy")) {
            if (args.get("--path-policy") != "follow" && args.get("--path-policy") != "stay") {
                throw std::invalid_argument("--path-policy takes follow or stay");
            }
            paths.policy = args.get("--path-policy") == "stay" ? RCS_PATH_POLICY_STAY : RCS_PATH_POLICY_FOLLOW_SYSTEM;
        }
        paths.standby = args.has("--standby");
    }
    if (args.has("--stun")) {
        std::vector<std::string> servers;
        std::stringstream list(args.get("--stun"));
        for (std::string server; std::getline(list, server, ',');) {
            servers.push_back(server);
        }
        config.setStunServers(servers);
    }

    if (!share && args.number("--viewers", 1) > 1) {
        if (args.has("--record")) {
            throw std::invalid_argument("--record takes one viewer");
        }
        return runViewers(args, config, args.number("--viewers", 1));
    }

    // HOST: the "host's own channel" is a connection to rcsdk_host_relay.
    std::unique_ptr<relay::RelayChannel> relayChannel;
    if (args.get("--transport") == "host") {
        const rcs_host_channel kind =
            args.has("--relay-datagram") ? RCS_HOST_CHANNEL_DATAGRAM : RCS_HOST_CHANNEL_STREAM;
        relayChannel =
            std::make_unique<relay::RelayChannel>(args.get("--relay"), args.get("--room", "rcsdk"), kind, share);
        config.use(rcsdk::hostTransport(*relayChannel, kind, args.number("--host-max-datagram", 0)));
    }

    rcsdk::Callbacks callbacks; // Frames are counted from the statistics.
    // --frames: the first decoded picture's layout and colour, once.
    std::atomic<bool> frameSeen{false};
    const std::string frames = args.get("--frames");
    if (frames != "" && frames != "gpu" && frames != "cpu") {
        throw std::invalid_argument("--frames takes gpu or cpu");
    }
    struct LastFrame { // --save-frame: kept from each picture, written at the end.
        std::string path;
        std::mutex mutex;
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> pixels;
    } last;
    last.path = args.get("--save-frame");
    if (!last.path.empty() && frames != "cpu") {
        throw std::invalid_argument("--save-frame needs --frames cpu");
    }
    if (!share && frames == "gpu") {
        callbacks.onGpuFrame = [&frameSeen](const rcsdk::GpuFrame& frame) {
            if (!frameSeen.exchange(true)) {
                const auto info = frame.info();
                std::cout << "frame: GPU " << layoutName(info.format) << " " << info.width << "x" << info.height
                          << " (" << colorText(info.color) << ")" << std::endl;
            }
        };
    } else if (!share && frames == "cpu") {
        callbacks.onFrame = [&frameSeen, &last](const rcs_frame& frame) {
            if (!frameSeen.exchange(true)) {
                std::cout << "frame: CPU " << layoutName(frame.format) << " " << frame.width << "x" << frame.height
                          << std::endl;
            }
            if (last.path.empty()) {
                return;
            }
            std::lock_guard lock(last.mutex);
            last.width = frame.width;
            last.height = frame.height;
            last.pixels.resize(size_t(frame.width) * frame.height * 4);
            for (uint32_t row = 0; row < frame.height; ++row) {
                std::memcpy(last.pixels.data() + size_t(row) * frame.width * 4,
                            frame.data + size_t(row) * frame.stride_bytes, size_t(frame.width) * 4);
            }
        };
    }
    // Data channels: text is printed; a blast (channel 7) is counted.
    std::atomic<uint64_t> blastReceived{0};
    // --receive-audio host: the other end's sound reaches on_audio only; its
    // level is printed (a sharer: each viewer's).
    struct Heard {
        uint64_t frames = 0;
        double energy = 0;
    };
    std::mutex heardMutex;
    std::map<rcs_viewer_id, Heard> heard;
    if (args.get("--receive-audio") == "host") {
        callbacks.onAudio = [&](const rcs_audio_frame& frame) {
            std::lock_guard lock(heardMutex);
            auto& h = heard[frame.viewer_id];
            h.frames += frame.frames;
            for (uint32_t i = 0; i < frame.frames * frame.channels; ++i) {
                h.energy += double(frame.samples[i]) * frame.samples[i];
            }
        };
    }
    if (args.has("--host-input")) { // The viewer's input, printed instead of executed.
        callbacks.onInput = [](const rcs_input_event& event) {
            if (event.type == RCS_INPUT_KEY && event.code == 44 && event.down) { // Space: --pattern-marker.
                ++patternMarker;
            }
            const char* names[] = {"",    "move", "relative", "button", "wheel",       "scroll",
                                   "key", "text", "touch",    "pen",    "release-all", "ctrl-alt-del"};
            std::cout << "input from viewer " << event.viewer_id << ": "
                      << (event.type <= RCS_INPUT_SECURE_ATTENTION ? names[event.type] : "?");
            if (event.type == RCS_INPUT_TEXT) {
                std::cout << " U+" << std::hex << std::uppercase << event.code << std::dec;
            } else if (event.type == RCS_INPUT_KEY || event.type == RCS_INPUT_BUTTON) {
                std::cout << " " << event.code << (event.down ? " down" : " up");
            } else if (event.type == RCS_INPUT_MOVE) {
                std::cout << " " << event.x << "," << event.y;
            } else if (event.type == RCS_INPUT_WHEEL || event.type == RCS_INPUT_SCROLL) {
                std::cout << " " << event.wheel_x << "," << event.wheel_y << " at " << event.x << "," << event.y;
            } else if (event.type == RCS_INPUT_PEN) {
                std::cout << " " << event.x << "," << event.y << (event.down ? " down" : " up") << " pressure "
                          << event.pressure << " tilt " << event.tilt_x << "," << event.tilt_y << " flags "
                          << event.pen;
            }
            std::cout << std::endl;
            return true;
        };
    }
    callbacks.onData = [&](const rcs_data_message& m) {
        if (m.channel == 7) {
            blastReceived += m.size;
            return;
        }
        std::cout << "data: channel " << m.channel << ", " << m.size << " bytes"
                  << (m.flags & RCS_DATA_UNRELIABLE ? " (unreliable)" : "") << ": "
                  << std::string(reinterpret_cast<const char*>(m.data), std::min<uint32_t>(m.size, 200)) << std::endl;
    };
    // P2P and RELAY: the "host's channel" for signals is a signal room of
    // rcsdk_host_relay (each line names the viewer it is from or for, so any
    // number of viewers), or a pair of files copied between the machines.
    const std::string signalOut = args.get("--signal-out");
    std::unique_ptr<relay::SignalBoard> board; // Made once the session is.
    const uint64_t viewerId = share ? 0 : newViewerId();
    callbacks.onSignal = [&board, share, viewerId, signalOut](const std::string& text, uint64_t route) {
        std::cout << (share ? "signal for viewer " + std::to_string(route) : std::string("signal")) << ": " << text
                  << std::endl;
        if (board) {
            board->send((share ? "share " + std::to_string(route) : "view " + std::to_string(viewerId)) + " " + text);
        }
        if (!signalOut.empty()) {
            std::ofstream(signalOut + ".part") << text;
            std::error_code error; // Replaces the last one (std::rename does not on Windows).
            std::filesystem::rename(signalOut + ".part", signalOut, error);
        }
    };
    // --admit-after N: the host decides on each viewer N seconds after it
    // came, as one that asks its user would: RCS_ADMIT_LATER here, then
    // rcs_session_admit_viewer from the loop below. --refuse-viewers REASON
    // refuses them instead.
    struct Waiting {
        rcs_viewer_id id;
        uint32_t control;
        std::chrono::steady_clock::time_point due;
    };
    std::mutex waitingMutex;
    std::vector<Waiting> waiting;
    const bool admitLater = share && args.has("--admit-after");
    const auto admitAfter = std::chrono::seconds(args.number("--admit-after", 0));
    const bool refuseViewers = share && args.has("--refuse-viewers");
    const std::string refusal = args.get("--refuse-viewers");
    const auto decide = [&](rcs_viewer_decision& decision) {
        if (refuseViewers) {
            decision.admission = RCS_REFUSE;
            std::snprintf(decision.reason, sizeof(decision.reason), "%s", refusal.c_str());
        }
    };
    if (admitLater || refuseViewers) {
        callbacks.onViewerJoin = [&](const rcs_viewer_info& viewer, rcs_viewer_decision& decision) {
            if (!admitLater) {
                decide(decision);
                return;
            }
            decision.admission = RCS_ADMIT_LATER;
            std::lock_guard lock(waitingMutex);
            waiting.push_back({viewer.viewer_id, decision.control, std::chrono::steady_clock::now() + admitAfter});
        };
    }
    // --power-ask: asked once the sharer said what it offers (RCS_POWER_PEER).
    std::atomic<bool> powerPeerHeard{false};
    std::atomic<rcs_session> sessionHandle{nullptr};
    callbacks.onEvent = [&files, &powerPeerHeard, &sessionHandle](const rcs_event& event) {
        static const char* names[] = {"idle",    "starting",     "listening", "connecting",
                                      "running", "reconnecting", "stopped",   "failed"};
        // A sharer's events that concern one viewer name it.
        const std::string who = event.viewer_id ? "[viewer " + std::to_string(event.viewer_id) + "] " : "";
        switch (event.type) {
        case RCS_EVENT_PIN:
            std::cout << "PIN handshake failed: " << event.pin.failures << " so far, " << event.pin.attempts_left
                      << " more allowed" << (event.pin.attempts_left ? "" : " (PINs refused until set again)")
                      << std::endl;
            break;
        case RCS_EVENT_ADMISSION: {
            const auto& a = event.admission;
            static const char* states[] = {"", "waits for the sharer's host", "admitted", "refused", "gone"};
            std::cout << who << "admission: " << states[a.state <= RCS_ADMISSION_GONE ? a.state : 0];
            if (a.state == RCS_ADMISSION_WAITING) {
                std::cout << (a.timeout_ms ? " (" + std::to_string(a.timeout_ms / 1000) + " s at most)" : "");
            } else if (a.result != RCS_OK) {
                std::cout << " (" << rcs_result_message(a.result) << ": " << a.message << ")";
            }
            std::cout << std::endl;
            break;
        }
        case RCS_EVENT_VIEWER: {
            const auto& v = event.viewer;
            if (v.joined) {
                std::cout << "viewer " << v.viewer_id << " joined from " << v.address << ":" << v.port
                          << (v.route ? ", route " + std::to_string(v.route) : std::string())
                          << (v.is_main ? " (main)" : "") << (v.control.allowed ? ", may control" : "") << std::endl;
            } else {
                std::cout << "viewer " << v.viewer_id << " left (" << rcs_result_message(v.result) << ")"
                          << (v.message[0] ? std::string(": ") + v.message : std::string()) << std::endl;
            }
            break;
        }
        case RCS_EVENT_STATE:
            std::cout << "state: " << (event.state.value <= RCS_STATE_FAILED ? names[event.state.value] : "?");
            if (event.state.result != RCS_OK) {
                std::cout << " (" << rcs_result_message(event.state.result) << ")";
            }
            std::cout << std::endl;
            break;
        case RCS_EVENT_CODEC:
            std::cout << (event.stream_index ? "stream " + std::to_string(event.stream_index) + " " : std::string())
                      << (event.codec.direction == RCS_CODEC_ENCODER ? "encoder: " : "decoder: ") << event.codec.name
                      << (event.codec.acceleration == RCS_ACCELERATION_SOFTWARE ? " (software)" : " (hardware)")
                      << (event.codec.temporal_layers > 1
                              ? ", " + std::to_string(event.codec.temporal_layers) + " temporal layers"
                              : std::string())
                      << std::endl;
            break;
        case RCS_EVENT_ENCODING: {
            const auto& e = event.encoding;
            std::cout << "stream " << event.stream_index << " encoding " << e.encoding << ": ";
            if (e.running) {
                std::cout << "started, " << e.width << "x" << e.height << " at " << e.frames_per_second << " FPS";
            } else {
                std::cout << "stopped" << (e.result != RCS_OK ? std::string(" (") + e.message + ")" : std::string());
            }
            std::cout << std::endl;
            break;
        }
        case RCS_EVENT_VIEWER_VIDEO: {
            const char* why[] = {"?", "joined", "path", "losses", "decoder", "host", "encoding ended"};
            const auto& v = event.viewer_video;
            std::cout << "viewer " << event.viewer_id << " gets encoding " << v.encoding << ", " << v.layers
                      << (v.layers == 1 ? " temporal layer (" : " temporal layers (")
                      << why[v.reason <= RCS_VIEWER_VIDEO_ENCODING ? v.reason : 0] << ", path "
                      << v.target_bps / 1000 << " kbit/s)" << std::endl;
            break;
        }
        case RCS_EVENT_CAPTURE: {
            const char* why[] = {"capture resumed", "capture paused (the desktop: locked, secure, asleep)",
                                 "capture paused (what is shared does not show)",
                                 "capture paused (the window or application is gone)", "capture paused (no agent)"};
            std::cout << (event.capture.paused <= RCS_CAPTURE_PAUSE_AGENT ? why[event.capture.paused] : "capture ?")
                      << std::endl;
            break;
        }
        case RCS_EVENT_MODULE: {
            const auto& m = event.module;
            if (std::strcmp(m.module, RCS_CLIPBOARD_MODULE) == 0 && m.kind == RCS_CLIPBOARD_COPY &&
                m.size == sizeof(rcs_clipboard_event)) {
                const auto& c = *static_cast<const rcs_clipboard_event*>(m.payload);
                std::cout << who << "clipboard " << (c.flow == RCS_CLIPBOARD_SEND ? "sent" : "placed") << ": "
                          << rcs_result_message(c.result) << ", formats 0x" << std::hex << c.formats
                          << ", left out 0x" << c.left_out << std::dec << ", " << c.bytes << " bytes"
                          << (c.message[0] ? std::string(" (") + c.message + ")" : "") << std::endl;
            } else if (std::strcmp(m.module, RCS_CLIPBOARD_MODULE) == 0 && m.kind == RCS_CLIPBOARD_PASTE &&
                       m.size == sizeof(rcs_clipboard_paste_event)) {
                const auto& p = *static_cast<const rcs_clipboard_paste_event*>(m.payload);
                std::cout << who << "clipboard files "
                          << (p.flow == RCS_CLIPBOARD_SEND ? "pasted there"
                              : p.ahead                    ? "fetched ahead"
                                                           : "pasted here")
                          << (p.cut ? " (cut there)" : "") << ": "
                          << (p.result == RCS_OK ? "file transfer " + std::to_string(p.transfer)
                                                 : std::string(rcs_result_message(p.result)) + " (" + p.message + ")")
                          << std::endl;
            } else if (std::strcmp(m.module, RCS_CLIPBOARD_MODULE) == 0 && m.kind == RCS_CLIPBOARD_PEER &&
                       m.size == sizeof(rcs_clipboard_peer)) {
                const auto& p = *static_cast<const rcs_clipboard_peer*>(m.payload);
                std::cout << who << "clipboard: the other end takes 0x" << std::hex << p.formats << std::dec
                          << ", up to " << p.max_bytes << " bytes" << std::endl;
            } else if (std::strcmp(m.module, EXAMPLE_POINTER_MODULE) == 0 && m.kind == EXAMPLE_POINTER_MOVED) {
                static std::atomic<uint64_t> moves{0};
                const auto& p = *static_cast<const example_pointer*>(m.payload);
                if (moves++ % 20 == 0) {
                    std::cout << who << "pointer: " << (p.visible ? std::to_string(p.x) + "," + std::to_string(p.y)
                                                                  : std::string("hidden"))
                              << " (" << moves << " moves)" << std::endl;
                }
            } else if (std::strcmp(m.module, RCS_TEXT_CARET_MODULE) == 0 && m.kind == RCS_TEXT_CARET_CHANGED &&
                       m.size == sizeof(rcs_text_caret)) {
                const auto& c = *static_cast<const rcs_text_caret*>(m.payload);
                if (c.visible) {
                    std::cout << "text caret: " << c.x << "," << c.y << " " << c.width << "x" << c.height
                              << std::endl;
                } else {
                    std::cout << "text caret: none" << std::endl;
                }
            } else if (std::strcmp(m.module, RCS_POWER_MODULE) == 0 && m.kind == RCS_POWER_CHANGED &&
                       m.size == sizeof(rcs_power_status)) {
                const auto& p = *static_cast<const rcs_power_status*>(m.payload);
                const char* modes[] = {"off", "system", "display"};
                std::cout << who << "keep awake: wants " << modes[p.wanted <= 2 ? p.wanted : 0] << ", holds "
                          << modes[p.held <= 2 ? p.held : 0]
                          << (p.result == RCS_OK ? "" : std::string(" (") + p.message + ")") << std::endl;
            } else if (std::strcmp(m.module, RCS_POWER_MODULE) == 0 && m.kind == RCS_POWER_ACTION &&
                       m.size == sizeof(rcs_power_action_event)) {
                const auto& a = *static_cast<const rcs_power_action_event*>(m.payload);
                const char* states[] = {"?", "refused", "accepted", "started", "failed"};
                std::cout << who << "power: request " << a.request << " to "
                          << powerActionNames[a.action <= RCS_POWER_SIGN_OUT ? a.action : 0]
                          << (a.flags & RCS_POWER_FORCE ? " (force)" : "") << " "
                          << states[a.state <= RCS_POWER_ACTION_FAILED ? a.state : 0]
                          << (a.state == RCS_POWER_ACTION_ACCEPTED ? ", in " + std::to_string(a.delay_ms) + " ms"
                                                                   : std::string())
                          << (a.result == RCS_OK ? std::string()
                                                 : std::string(" (") + rcs_result_message(a.result) + ": " +
                                                       a.message + ")")
                          << std::endl;
            } else if (std::strcmp(m.module, RCS_POWER_MODULE) == 0 && m.kind == RCS_POWER_PEER &&
                       m.size == sizeof(rcs_power_peer)) {
                const auto& p = *static_cast<const rcs_power_peer*>(m.payload);
                std::cout << "power: the sharer offers " << powerActionList(p.actions) << "; it "
                          << (p.wake_info ? "tells " + std::to_string(p.interfaces) + " interfaces that can wake it"
                                          : std::string("tells no wake information"))
                          << std::endl;
                std::vector<rcs_power_interface> list(p.interfaces);
                uint32_t count = 0;
                if (!list.empty() && rcs_power_get_peer_interfaces(sessionHandle.load(), list.data(), sizeof(rcs_power_interface),
                                                                   uint32_t(list.size()), &count) == RCS_OK) {
                    for (const auto& it : list) {
                        printInterface(it);
                    }
                }
                powerPeerHeard = true;
            } else if (std::strcmp(m.module, RCS_TEXT_CARET_MODULE) == 0 && m.kind == RCS_TEXT_CARET_TELLS &&
                       m.size == sizeof(uint32_t)) {
                std::cout << "text caret: the sharer " << (*static_cast<const uint32_t*>(m.payload) ? "tells" : "does not tell")
                          << std::endl;
            } else if (std::strcmp(m.module, RCS_FILE_TRANSFER_MODULE) == 0 && m.kind >= RCS_MODULE_EVENT_OWN) {
                files.event(event.viewer_id, m, who);
            } else if (m.kind < RCS_MODULE_EVENT_OWN) {
                const char* kinds[] = {"?", "active", "inactive", "has no machine half"};
                std::cout << who << "module " << m.module << ": " << kinds[m.kind <= 3 ? m.kind : 0]
                          << (m.result == RCS_OK ? "" : std::string(" (") + m.message + ")") << std::endl;
            }
            break;
        }
        case RCS_EVENT_PEER:
            std::cout << who << "peer: platform " << event.peer.platform << ", codecs 0x" << std::hex
                      << event.peer.codecs << ", audio codecs 0x" << event.peer.send_audio_codecs << " out, 0x"
                      << event.peer.receive_audio_codecs << " in, input 0x" << event.peer.input_types << std::dec
                      << (event.peer.sends_audio ? ", sends sound" : "")
                      << (event.peer.receives_audio ? ", receives sound" : "")
                      << (event.peer.has_public_key
                              ? ", device key " + hexOf(event.peer.public_key, RCS_PUBLIC_KEY_SIZE)
                              : std::string())
                      << std::endl;
            break;
        case RCS_EVENT_STREAM: {
            const auto& info = event.stream.info;
            const std::string which = event.stream_index ? "stream " + std::to_string(event.stream_index) : "stream";
            if (event.stream.changes & RCS_STREAM_ENDED) {
                std::cout << which << " ended" << std::endl;
                break;
            }
            std::cout << who << which << ": " << codecName(info.codec) << " " << formatName(info.format) << " ("
                      << colorText(info.color) << ") " << info.width << "x" << info.height << " at "
                      << info.frames_per_second << " FPS, " << info.bitrate_bps / 1000 << " kbit/s, audio "
                      << audioName(info.audio_codec) << ", content " << info.content_x << "," << info.content_y << " "
                      << info.content_width << "x" << info.content_height << " of " << info.display_width << "x"
                      << info.display_height
                      << (info.desktop_width ? " at " + std::to_string(info.desktop_x) + "," +
                                                   std::to_string(info.desktop_y) + " on the desktop"
                                             : std::string())
                      << (info.layers > 1 ? ", temporal layers " + std::to_string(info.received_layers) + " of " +
                                                std::to_string(info.layers)
                                          : std::string())
                      << " (changes 0x" << std::hex << event.stream.changes << std::dec << ")" << std::endl;
            break;
        }
        case RCS_EVENT_DESKTOP:
            std::cout << "desktop: "
                      << (event.desktop.kind == RCS_DESKTOP_USER ? "the user's"
                          : event.desktop.kind == RCS_DESKTOP_SECURE
                              ? (event.desktop.user_logged_on ? "secure (locked or a prompt)"
                                                              : "secure (sign-in screen)")
                              : "unknown")
                      << std::endl;
            break;
        case RCS_EVENT_AGENT:
            std::cout << "agent: " << (event.agent.attached ? "attached" : "detached") << ", process "
                      << event.agent.process_id << ", system session " << event.agent.os_session << ", "
                      << event.agent.displays << " display(s)" << std::endl;
            break;
        case RCS_EVENT_PAUSE: {
            const auto named = [](uint32_t what) {
                return std::string(what & RCS_PAUSE_VIDEO ? "video" : "") +
                       (what == (RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO) ? " and " : "") +
                       (what & RCS_PAUSE_AUDIO ? "audio" : "") + (what ? "" : "nothing");
            };
            std::cout << "paused: " << named(event.pause.paused) << " (the host: " << named(event.pause.by_host)
                      << ", the background: " << named(event.pause.by_background)
                      << ", the sharer: " << named(event.pause.by_peer) << ")" << std::endl;
            break;
        }
        case RCS_EVENT_AUDIO:
            std::cout << who << (event.audio.direction == RCS_AUDIO_SENDING ? "sound out " : "sound in ")
                      << audioName(event.audio.codec)
                      << (event.audio.voice_processing ? " (voice processing)" : "")
                      << (event.audio.active ? ": active" : std::string(": inactive (") + event.audio.message + ")")
                      << std::endl;
            break;
        case RCS_EVENT_PATH: {
            static const char* changes[] = {"?", "changing", "changed", "lost", "standby"};
            static const char* causes[] = {"?", "network", "stalled", "peer", "rebinding", "settings", "host"};
            static const char* routes[] = {"none", "direct", "NAT", "relay", "host", "forwarder"};
            const auto& p = event.path;
            const auto end = [&](const rcs_path_info& info) {
                return std::string(routes[info.route <= RCS_ROUTE_FORWARDER ? info.route : 0]) + " " + info.local_address +
                       " -> " + info.remote_address + (info.interface_name[0] ? " over " : "") + info.interface_name;
            };
            std::cout << who << "path " << changes[p.change <= RCS_PATH_STANDBY ? p.change : 0] << " ("
                      << causes[p.cause <= RCS_PATH_CAUSE_HOST ? p.cause : 0] << ")";
            if (p.change == RCS_PATH_CHANGED || p.change == RCS_PATH_STANDBY) {
                std::cout << ": " << end(p.to) << ", " << p.interruption_ns / 1000000 << " ms without"
                          << (p.signaled ? ", found through the signalling" : "");
            }
            if (p.result != RCS_OK) {
                std::cout << " (" << rcs_result_message(p.result) << ": " << p.message << ")";
            }
            std::cout << std::endl;
            break;
        }
        case RCS_EVENT_STALLS: {
            const auto& t = event.stalls;
            std::cout << who << (t.from_peer ? "the other end's path " : "path ");
            if (t.period_ms) {
                std::cout << "stalls about " << t.stall_ms << " ms every " << t.period_ms << " ms";
            } else {
                std::cout << "stalls no longer come at a steady pace";
            }
            if (t.wifi.name[0]) {
                std::cout << " (this end's Wi-Fi " << t.wifi.name << ": channel " << t.wifi.channel
                          << (t.wifi.other_channel ? ", AWDL on another channel" : "") << ")";
            }
            std::cout << std::endl;
            break;
        }
        default:
            break;
        }
    };
    // --record: the packets as they arrive, written to files (examples/common/recorder.hpp).
    std::unique_ptr<recorder::Recorder> recording; // Outlives the session, which calls it.
    if (args.has("--record-only") && !args.has("--record")) {
        throw std::invalid_argument("--record-only needs --record PREFIX");
    }
    if (args.has("--record")) {
        if (share) {
            throw std::invalid_argument("--record is a viewer's (a sharer's packets are the same)");
        }
        recording = std::make_unique<recorder::Recorder>(args.get("--record"));
        config.use(rcsdk::encodedOutput(*recording, !args.has("--record-only")));
    }
    rcsdk::Session session(config, std::move(callbacks));
    sessionHandle = session.handle();
    // --send-audio host: a 440 Hz tone as the host's sound.
    std::atomic<bool> toneGoing{args.get("--send-audio") == "host"};
    std::thread tone([&] {
        std::vector<float> block(480 * 2);
        uint64_t produced = 0;
        for (auto next = std::chrono::steady_clock::now(); toneGoing && !interrupted;
             next += std::chrono::milliseconds(10)) {
            for (uint32_t i = 0; i < 480; ++i) {
                block[i * 2] = block[i * 2 + 1] = float(0.3 * std::sin(2 * 3.14159265358979 * 440 * double(produced + i) / 48000));
            }
            produced += 480;
            session.pushAudio(block.data(), 480); // INVALID_STATE until running: dropped.
            std::this_thread::sleep_until(next + std::chrono::milliseconds(10));
        }
    });
    struct ToneStop {
        std::atomic<bool>& going;
        std::thread& thread;
        ~ToneStop() {
            going = false;
            thread.join();
        }
    } toneStop{toneGoing, tone};
    files.attach(session.handle());
    if (args.has("--signal-relay")) {
        if (!share) {
            std::cout << "viewer id " << viewerId << std::endl;
        }
        const auto line = [&session, share, viewerId](const std::string& text) {
            uint64_t id = 0;
            std::string signal;
            if (!parseSignalLine(text, share ? "view" : "share", id, signal) || (!share && id != viewerId)) {
                return;
            }
            const auto result = session.signal(signal, share ? id : 0);
            std::cout << (share ? "signal from viewer " + std::to_string(id) : std::string("answer")) << ": "
                      << rcs_result_message(result) << std::endl;
        };
        board = std::make_unique<relay::SignalBoard>(args.get("--signal-relay"), args.get("--room", "rcsdk"), line);
    }
    struct BoardStop { // No line reaches the session once this goes, before it does.
        std::unique_ptr<relay::SignalBoard>& board;
        ~BoardStop() { board.reset(); }
    } boardStop{board};
    if (const auto result = session.start(); result != RCS_OK) {
        std::cerr << "Start failed: " << rcs_result_message(result) << ": " << session.failure().message << "\n";
        return 1;
    }
    // The relay delivers into the session until this guard goes, before the session does.
    struct RelayStop {
        relay::RelayChannel* channel;
        ~RelayStop() {
            if (channel) {
                channel->stop();
            }
        }
    } relayStop{relayChannel.get()};
    if (relayChannel) {
        relayChannel->start(session);
    }
    auto pattern = share && args.has("--host-pattern") ? std::make_unique<PatternSource>(session, args.has("--pattern-marker")) : nullptr;
    auto encodedPattern =
        share && args.has("--encoded-pattern") ? std::make_unique<EncodedPattern>(session) : nullptr;
    const std::string signalIn = args.get("--signal-in");
    std::string lastSignal; // Each new signal in the file goes on: a sharer that keeps listening meets new viewers.
    std::string typing = args.get("--type");
    std::string textToSend = args.get("--text");
    bool attention = args.has("--send-attention");
    std::string powerAsk = share ? std::string() : args.get("--power-ask"); // ACTION[,force]
    const bool powerForce = powerAsk.size() > 6 && powerAsk.compare(powerAsk.size() - 6, 6, ",force") == 0;
    const rcs_power_action powerAction =
        powerAsk.empty() ? 0 : powerActionNamed(powerForce ? powerAsk.substr(0, powerAsk.size() - 6) : powerAsk);
    // --pause WHAT,AFTER,FOR: the host pauses what it says for a while, as a
    // window that went to the background would.
    uint32_t pauseWhat = 0, pauseAfter = 0, pauseFor = 0;
    if (args.has("--pause")) {
        char what[16] = {};
        if (std::sscanf(args.get("--pause").c_str(), "%15[a-z],%u,%u", what, &pauseAfter, &pauseFor) != 3) {
            throw std::invalid_argument("--pause takes video|audio|both,AFTER,FOR (seconds)");
        }
        pauseWhat = std::string(what) == "video"   ? RCS_PAUSE_VIDEO
                    : std::string(what) == "audio" ? RCS_PAUSE_AUDIO
                    : std::string(what) == "both"  ? RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO
                                                   : throw std::invalid_argument("--pause: video, audio or both");
    }
    enum class Pausing { before, paused, resumed } pausing = Pausing::before;
    std::chrono::steady_clock::time_point runningSince{};

    const uint32_t seconds = args.number("--seconds", 0);
    const auto started = std::chrono::steady_clock::now();
    std::string text = args.get("--send-text");
    std::string clipboardText = args.get("--clipboard-text"); // Sent once the other end takes copies.
    std::vector<std::string> clipboardFiles;                  // The same, of --clipboard-file paths.
    for (const auto& path : args.all("--clipboard-file")) {
        const auto full = std::filesystem::absolute(path).u8string();
        clipboardFiles.emplace_back(full.begin(), full.end());
    }
    uint64_t blastLeft = uint64_t(args.number("--data-blast", 0)) << 20;
    const uint64_t blastTotal = blastLeft;
    std::chrono::steady_clock::time_point blastStarted{};
    bool blastReported = false;
    uint64_t blastSeen = 0;
    const std::vector<uint8_t> chunk(1u << 20, 0x5A);
    auto nextReport = started + 1s;
    const bool recordOnly = args.has("--record-only"); // Nothing is decoded: count what arrives.
    const auto produced = [&] {
        return share        ? session.stats<rcs_video_send_stats>().encoded_frames
               : recordOnly ? session.stats<rcs_video_receive_stats>().received_packets
                            : session.stats<rcs_video_receive_stats>().decoded_frames;
    };
    auto nextKeyframeRequest = started;
    uint64_t lastProduced = 0;
    std::array<uint64_t, RCS_MAX_STREAMS> streamFrames{}; // Of the other streams, as last reported.
    bool thumbnailTaken = false; // --session-thumbnail: taken...
    auto nextThumbnail = started;   // ...asked for next then...
    std::string thumbnailProblem;   // ...and why not, as last printed.
    while (!interrupted) {
        waitFor(50ms);
        const auto now = std::chrono::steady_clock::now();
        const rcs_state state = session.state();
        if (state == RCS_STATE_FAILED || state == RCS_STATE_STOPPED) {
            break;
        }
        files.poll();
        if (!signalIn.empty()) {
            std::ifstream file(signalIn);
            std::string text;
            if (file && std::getline(file, text) && !text.empty() && text != lastSignal) {
                const auto result = session.signal(text);
                std::cout << "peer signal: " << rcs_result_message(result) << std::endl;
                lastSignal = text;
            }
        }
        if (seconds && now - started >= std::chrono::seconds(seconds)) {
            break;
        }
        if (!powerAsk.empty() && powerPeerHeard) {
            uint64_t request = 0;
            if (rcs_power_request_action(session.handle(), powerAction, powerForce ? RCS_POWER_FORCE : 0, &request) ==
                RCS_OK) {
                std::cout << "power: asked the sharer's machine to " << powerAsk << " (request " << request << ")"
                          << std::endl;
            } else {
                std::cout << "power: could not ask: " << rcsdk::lastCallError() << std::endl;
            }
            powerAsk.clear();
        }
        if (admitLater) {
            std::vector<Waiting> due;
            {
                std::lock_guard lock(waitingMutex);
                const auto later = std::partition(waiting.begin(), waiting.end(), [&](const Waiting& w) {
                    return w.due > now;
                });
                due.assign(later, waiting.end());
                waiting.erase(later, waiting.end());
            }
            for (const auto& w : due) {
                rcs_viewer_decision decision = RCS_VIEWER_DECISION_INIT;
                decision.control = w.control;
                decide(decision);
                std::cout << "viewer " << w.id << (refuseViewers ? " refused: " : " admitted: ")
                          << rcs_result_message(session.admitViewer(w.id, decision)) << std::endl;
            }
        }
        if (state == RCS_STATE_RUNNING && (!clipboardText.empty() || !clipboardFiles.empty())) {
            rcs_clipboard_peer peer = RCS_CLIPBOARD_PEER_INIT;
            // A sharer's first viewer (1), or a viewer's sharer (0).
            const uint32_t wanted = (clipboardText.empty() ? 0u : 1u << RCS_CLIPBOARD_TEXT) |
                                    (clipboardFiles.empty() ? 0u : 1u << RCS_CLIPBOARD_FILES);
            if (rcs_clipboard_get_peer(session.handle(), share ? 1 : 0, &peer) == RCS_OK &&
                (peer.formats & wanted) == wanted) {
                const rcs_clipboard_item item{RCS_CLIPBOARD_TEXT, uint32_t(clipboardText.size()),
                                              reinterpret_cast<const uint8_t*>(clipboardText.data())};
                rcs_clipboard_content copy = RCS_CLIPBOARD_CONTENT_INIT;
                copy.count = clipboardText.empty() ? 0 : 1;
                copy.items = &item;
                // --clipboard-file: files copied by path (on this machine, as file transfer reads them).
                std::vector<std::string> names;
                std::vector<rcs_clipboard_file> entries;
                for (const auto& path : clipboardFiles) {
                    names.push_back(std::filesystem::path(path).filename().string());
                }
                for (size_t i = 0; i < clipboardFiles.size(); ++i) {
                    const bool directory = std::filesystem::is_directory(clipboardFiles[i]);
                    entries.push_back({names[i].c_str(), rcs_file_kind(directory ? RCS_FILE_DIRECTORY : RCS_FILE_REGULAR),
                                       directory ? 0 : uint64_t(std::filesystem::file_size(clipboardFiles[i])), 0,
                                       clipboardFiles[i].c_str()});
                }
                rcs_clipboard_files listed = RCS_CLIPBOARD_FILES_INIT;
                listed.count = uint32_t(entries.size());
                listed.files = entries.data();
                copy.files = entries.empty() ? nullptr : &listed;
                const rcs_result sent = rcs_clipboard_send(session.handle(), &copy);
                std::cout << "clipboard sent: " << rcs_result_message(sent)
                          << (sent == RCS_OK ? std::string() : " (" + rcsdk::lastCallError() + ")") << std::endl;
                clipboardText.clear();
                clipboardFiles.clear();
            }
        }
        if (state == RCS_STATE_RUNNING && !text.empty() && session.sendData(1, text) == RCS_OK) {
            text.clear();
        }
        if (pauseWhat && state == RCS_STATE_RUNNING && runningSince == std::chrono::steady_clock::time_point{}) {
            runningSince = now;
        }
        if (pauseWhat && pausing == Pausing::before && runningSince != std::chrono::steady_clock::time_point{} &&
            now - runningSince >= std::chrono::seconds(pauseAfter)) {
            std::cout << "pause: " << rcs_result_message(session.pause(pauseWhat)) << std::endl;
            pausing = Pausing::paused;
        } else if (pausing == Pausing::paused &&
                   now - runningSince >= std::chrono::seconds(pauseAfter) + std::chrono::seconds(pauseFor)) {
            std::cout << "resume: " << rcs_result_message(session.resume(pauseWhat)) << std::endl;
            pausing = Pausing::resumed;
        }
        // A recording starts, and goes on after lost pictures, at a keyframe: ask for one.
        if (recording && state == RCS_STATE_RUNNING && now >= nextKeyframeRequest && recording->waitingForKeyframe()) {
            session.requestKeyframe();
            nextKeyframeRequest = now + 1s;
        }
        if (share && args.has("--session-thumbnail") && !thumbnailTaken && now >= nextThumbnail) {
            nextThumbnail = now + std::chrono::seconds(1);
            std::vector<uint8_t> pixels(320 * 320 * 4);
            rcs_picture picture = RCS_PICTURE_INIT;
            picture.data = pixels.data();
            picture.capacity = pixels.size();
            const rcs_capture_config what = RCS_CAPTURE_CONFIG_INIT;
            const auto asked = std::chrono::steady_clock::now();
            const rcs_result result = rcs_thumbnail_capture(session.handle(), &what, 320, 320, &picture);
            const auto took =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - asked);
            if (result == RCS_OK) {
                thumbnailTaken = true;
                pixels.resize(size_t(picture.data_size));
                writeBmp(args.get("--session-thumbnail"), picture.width, picture.height, pixels);
                std::cout << "session thumbnail: " << picture.width << "x" << picture.height << " in " << took.count()
                          << " ms -> " << args.get("--session-thumbnail") << std::endl;
            } else if (const std::string why = rcsdk::lastCallError(); why != thumbnailProblem) {
                thumbnailProblem = why; // INVALID_STATE until its machine's half runs.
                std::cout << "session thumbnail: " << rcs_result_message(result) << " (" << why << ")" << std::endl;
            }
        }
        if (!share && state == RCS_STATE_RUNNING && args.has("--pointer")) { // Round the middle, once in 4 s.
            const double turn = std::chrono::duration<double>(now - started).count() * 2 * 3.14159265 / 4;
            const example_pointer where{1, uint32_t(32768 + 16000 * std::cos(turn)),
                                        uint32_t(32768 + 16000 * std::sin(turn))};
            example_pointer_point(session.handle(), &where); // INVALID_STATE until it is active.
        }
        if (!share && state == RCS_STATE_RUNNING && session.controlInfo().granted) {
            if (attention) {
                rcs_input_event event = RCS_INPUT_EVENT_INIT;
                event.type = RCS_INPUT_SECURE_ATTENTION;
                std::cout << "Ctrl+Alt+Del: " << rcs_result_message(session.sendInput(event)) << std::endl;
                attention = false;
            }
            if (!typing.empty()) {
                typeText(session, typing);
                std::cout << "typed " << typing.size() << " characters" << std::endl;
                typing.clear();
            }
            if (!textToSend.empty()) {
                std::cout << "sent " << sendText(session, textToSend) << " text events" << std::endl;
                textToSend.clear();
            }
        }
        while (state == RCS_STATE_RUNNING && blastLeft) { // Fills the queue; waits when it is full.
            if (blastStarted == std::chrono::steady_clock::time_point{}) {
                blastStarted = now;
            }
            const uint32_t size = uint32_t(std::min<uint64_t>(chunk.size(), blastLeft));
            if (session.sendData(7, chunk.data(), size) != RCS_OK) {
                break;
            }
            blastLeft -= size;
        }
        if (blastTotal && !blastLeft && !blastReported && session.stats<rcs_data_stats>().queued_bytes == 0) {
            const double took = std::chrono::duration<double>(now - blastStarted).count();
            std::cout << "data blast: " << (blastTotal >> 20) << " MiB queued and sent in " << took << " s ("
                      << double(blastTotal) / took / 1e6 << " MB/s)" << std::endl;
            blastReported = true;
        }

        if (now >= nextReport && state == RCS_STATE_RUNNING) {
            const uint64_t frames = produced();
            const auto received = session.stats<rcs_video_receive_stats>();
            std::cout << codecName(session.streamInfo().codec) << ' '
                      << (share ? "encoded " : recordOnly ? "received " : "decoded ")
                      << frames - lastProduced << " fps, network "
                      << session.stats<rcs_network_stats>().bytes / 1'000'000 << " MB, dropped "
                      << received.dropped_packets
                      << (received.decoder_recoveries
                              ? ", decoder recoveries " + std::to_string(received.decoder_recoveries)
                              : std::string());
            if (share) {
                const auto sent = session.stats<rcs_video_send_stats>();
                std::cout << ", encode " << sent.last_encode_ns / 1e6 << " ms, capture to packet "
                          << sent.last_pipeline_ns / 1e6 << " ms";
                const auto audio = session.stats<rcs_audio_send_stats>();
                if (audio.active) {
                    std::cout << ", audio " << audio.encoded_packets << " packets";
                }
            } else {
                const auto audio = session.stats<rcs_audio_receive_stats>();
                if (audio.active) {
                    std::cout << ", audio buffer " << audio.buffer_ms << "/" << audio.target_ms << " ms, lost "
                              << audio.lost_packets << ", recovered " << audio.recovered_packets << ", underruns "
                              << audio.underruns << " (device " << audio.device_underruns << "), queued on the path "
                              << session.stats<rcs_network_stats>().sound_queue_us / 1000.0 << " ms";
                }
                if (const auto network = session.stats<rcs_network_stats>(); network.stalls) {
                    std::cout << ", stalls " << network.stalls << " in 10 s (median " << network.stall_ms
                              << " ms, longest " << network.longest_stall_ms << " ms"
                              << (network.stall_period_ms ? ", every " + std::to_string(network.stall_period_ms) + " ms"
                                                          : std::string())
                              << ")";
                }
            }
            if (const uint64_t seen = blastReceived; seen != blastSeen) {
                std::cout << ", data received " << (seen >> 20) << " MiB";
                blastSeen = seen;
            }
            std::cout << std::endl;
            for (uint32_t stream = 1; stream < RCS_MAX_STREAMS; ++stream) { // More streams (--streams).
                const auto info = session.streamInfo(stream);
                if (!info.codec) {
                    continue;
                }
                const uint64_t count = share ? session.streamStats<rcs_video_send_stats>(stream).encoded_frames
                                             : session.streamStats<rcs_video_receive_stats>(stream).decoded_frames;
                std::cout << "  stream " << stream << ": " << info.width << "x" << info.height << ", "
                          << count - streamFrames[stream] << " fps" << std::endl;
                streamFrames[stream] = count;
            }
            if (share) { // Each viewer's own path.
                for (const auto& viewer : session.viewers()) {
                    const auto network = session.viewerStats<rcs_network_stats>(viewer.viewer_id);
                    std::cout << "  viewer " << viewer.viewer_id << (viewer.is_main ? " (main)" : "") << ": rtt "
                              << network.round_trip_ns / 1e6 << " ms, arriving " << network.received_bps / 1000
                              << " kbit/s, loss " << network.loss_permille << " per mille, queueing "
                              << network.queue_delay_us / 1000 << " ms, parity " << network.fec_percent << "%, path "
                              << network.path_bps / 1000 << " kbit/s, sound waited " << network.sound_wait_us / 1000.0
                              << " ms, " << network.bytes / 1'000'000 << " MB" << std::endl;
                }
            }
            {
                std::lock_guard lock(heardMutex);
                for (auto& [viewer, h] : heard) {
                    std::cout << "  heard" << (viewer ? " from viewer " + std::to_string(viewer) : std::string()) << ": "
                              << h.frames << " frames, rms "
                              << (h.frames ? std::sqrt(h.energy / double(h.frames * 2)) : 0.0) << std::endl;
                    h = {};
                }
            }
            lastProduced = frames;
            nextReport = now + 1s;
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const bool failed = session.state() == RCS_STATE_FAILED;
    if (failed) {
        const auto failure = session.failure();
        std::cerr << "Session failed (" << rcs_result_message(failure.result) << "): " << failure.message << "\n";
    }
    pattern.reset();
    if (encodedPattern) {
        std::cout << "encoded pattern: " << encodedPattern->pushed() << " packets pushed, " << encodedPattern->skipped()
                  << " pictures skipped while the path was full" << std::endl;
        encodedPattern.reset();
    }
    if (rawDecoder.packets || rawDecoder.wrongSize) {
        std::cout << "host decoder: " << rawDecoder.packets << " packets, " << rawDecoder.wrongSize
                  << " of the wrong size, latest mean colour 0x" << std::hex << rawDecoder.colour << std::dec
                  << std::endl;
    }
    const std::string viewers = share ? viewersJson(session) : "[]"; // While they are still connected.
    session.stop(args.has("--restart-notice") ? RCS_STOP_RESTART : RCS_STOP_NORMAL);
    if (recording) {
        recording->report(std::cout);
    }
    if (!last.path.empty() && last.width) {
        writeBmp(last.path, last.width, last.height, last.pixels);
        std::cout << "last picture: " << last.width << "x" << last.height << " -> " << last.path << std::endl;
    }
    if (args.has("--report")) {
        writeReport(args.get("--report"), session, share, elapsed, produced(), viewers);
    }
    return failed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { interrupted = true; });
    std::signal(SIGTERM, [](int) { interrupted = true; });
    try {
        // The official modules this program offers (--clipboard, --text-caret,
        // --session-thumbnail, --send-file and --receive-dir), also to a sharer
        // whose agent this process is (--agent).
        rcsdk::registerModule(*rcs_clipboard_module());
        rcsdk::registerModule(*rcs_file_transfer_module());
        rcsdk::registerModule(*rcs_text_caret_module());
        rcsdk::registerModule(*rcs_thumbnail_module());
        rcsdk::registerModule(*rcs_power_module());
        rcsdk::registerModule(*example_pointer_module()); // A module of anyone's, the same way.
        const auto args = parse(argc, argv);
        if (args.has("--help") || argc == 1) {
            usage();
            return 0;
        }
        if (args.has("--info")) {
            return printInfo();
        }
        if (args.has("--list-windows")) {
            return printWindows();
        }
        if (args.has("--wake-info")) {
            return printWakeInfo();
        }
        if (args.has("--wake")) {
            return sendWake(args);
        }
        if (args.has("--list-audio-devices")) {
            std::vector<rcs_audio_device_info> devices(64);
            uint32_t count = 0;
            if (rcs_list_audio_devices(devices.data(), sizeof(rcs_audio_device_info), 64, &count) != RCS_OK) {
                std::cerr << "Listing sound devices failed: " << rcsdk::lastCallError() << "\n";
                return 1;
            }
            static const char* permissions[] = {"unavailable", "required", "granted"};
            std::cout << "microphone permission: " << permissions[std::min(rcs_microphone_permission(), 2u)] << "\n";
            for (uint32_t i = 0; i < count; ++i) {
                const auto& d = devices[i];
                std::cout << (d.input ? "input  " : "output ") << d.device_id << (d.is_default ? " (default) " : " ")
                          << d.name << "\n";
            }
            return 0;
        }
        if (args.has("--thumbnail")) {
            return writeThumbnail(args);
        }
        if (args.has("--generate-key")) {
            return generateKey(args.get("--generate-key"));
        }
        if (args.has("--generate-device-key")) {
            return generateDeviceKey(args.get("--generate-device-key"));
        }
        if (args.has("--generate-pin")) {
            std::cout << rcsdk::generatePin(args.number("--generate-pin", 6)) << std::endl;
            return 0;
        }
        if (args.has("--agent")) {
            return runAgent(args);
        }
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 2;
    }
}
