//===-- lldb-platform.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <cerrno>
#if defined(__APPLE__)
#include <netinet/in.h>
#endif
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <fstream>
#include <optional>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include "Acceptor.h"
#include "LLDBServerUtilities.h"
#include "Plugins/Process/gdb-remote/GDBRemoteCommunicationServerPlatform.h"
#include "Plugins/Process/gdb-remote/ProcessGDBRemoteLog.h"
#include "lldb/Host/ConnectionFileDescriptor.h"
#include "lldb/Host/HostGetOpt.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Host/Socket.h"
#include "lldb/Host/common/TCPSocket.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Status.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::lldb_server;
using namespace lldb_private::process_gdb_remote;
using namespace llvm;

// option descriptors for getopt_long_only()

static int g_debug = 0;
static int g_verbose = 0;
static int g_server = 0;

static struct option g_long_options[] = {
    {"debug", no_argument, &g_debug, 1},
    {"verbose", no_argument, &g_verbose, 1},
    {"log-file", required_argument, nullptr, 'l'},
    {"log-channels", required_argument, nullptr, 'c'},
    {"listen", required_argument, nullptr, 'L'},
    {"port-offset", required_argument, nullptr, 'p'},
    {"gdbserver-port", required_argument, nullptr, 'P'},
    {"min-gdbserver-port", required_argument, nullptr, 'm'},
    {"max-gdbserver-port", required_argument, nullptr, 'M'},
    {"socket-file", required_argument, nullptr, 'f'},
    {"server", no_argument, &g_server, 1},
    {"child-platform-fd", required_argument, nullptr, 2},
    {nullptr, 0, nullptr, 0}};

#if defined(__APPLE__)
#define LOW_PORT (IPPORT_RESERVED)
#define HIGH_PORT (IPPORT_HIFIRSTAUTO)
#else
#define LOW_PORT (1024u)
#define HIGH_PORT (49151u)
#endif

#if !defined(_WIN32)
// Watch for signals
static void signal_handler(int signo) {
  switch (signo) {
  case SIGHUP:
    // Use SIGINT first, if that does not work, use SIGHUP as a last resort.
    // And we should not call exit() here because it results in the global
    // destructors to be invoked and wreaking havoc on the threads still
    // running.
    llvm::errs() << "SIGHUP received, exiting lldb-server...\n";
    abort();
    break;
  }
}
#endif

static void display_usage(const char *progname, const char *subcommand) {
  fprintf(stderr, "Usage:\n  %s %s [--log-file log-file-name] [--log-channels "
                  "log-channel-list] [--port-file port-file-path] --server "
                  "--listen port\n",
          progname, subcommand);
  exit(0);
}

static Status save_socket_id_to_file(const std::string &socket_id,
                                     const FileSpec &file_spec) {
  FileSpec temp_file_spec(file_spec.GetDirectory().GetStringRef());
  Status error(llvm::sys::fs::create_directory(temp_file_spec.GetPath()));
  if (error.Fail())
    return Status("Failed to create directory %s: %s",
                  temp_file_spec.GetPath().c_str(), error.AsCString());

  Status status;
  if (auto Err = llvm::writeToOutput(file_spec.GetPath(),
                                     [&socket_id](llvm::raw_ostream &OS) {
                                       OS << socket_id;
                                       return llvm::Error::success();
                                     }))
    return Status("Failed to atomically write file %s: %s",
                  file_spec.GetPath().c_str(),
                  llvm::toString(std::move(Err)).c_str());
  return status;
}

static void client_handle(GDBRemoteCommunicationServerPlatform &platform,
                          const lldb_private::Args &args) {
  if (!platform.IsConnected())
    return;

  if (args.GetArgumentCount() > 0) {
    lldb::pid_t pid = LLDB_INVALID_PROCESS_ID;
    std::optional<uint16_t> port;
    std::string socket_name;
    Status error = platform.LaunchGDBServer(args,
                                            "", // hostname
                                            pid, port, socket_name);
    if (error.Success())
      platform.SetPendingGdbServer(pid, *port, socket_name);
    else
      fprintf(stderr, "failed to start gdbserver: %s\n", error.AsCString());
  }

  bool interrupt = false;
  bool done = false;
  Status error;
  while (!interrupt && !done) {
    if (platform.GetPacketAndSendResponse(std::nullopt, error, interrupt,
                                          done) !=
        GDBRemoteCommunication::PacketResult::Success)
      break;
  }

  printf("Disconnected.\n");
}

static GDBRemoteCommunicationServerPlatform::PortMap gdbserver_portmap;
static std::mutex gdbserver_portmap_mutex;

static void spawn_process_reaped(lldb::pid_t pid, int signal, int status) {
  std::lock_guard<std::mutex> guard(gdbserver_portmap_mutex);
  gdbserver_portmap.FreePortForProcess(pid);
}

static Status spawn_process(const char *progname, const Socket *conn_socket,
                            uint16_t gdb_port, uint16_t port_offset,
                            const lldb_private::Args &args,
                            const std::string &log_file,
                            const StringRef log_channels) {
  Status error;
  SharedSocket shared_socket(conn_socket, error);
  if (error.Fail())
    return error;

  ProcessLaunchInfo launch_info;

  FileSpec self_spec(progname, FileSpec::Style::native);
  launch_info.SetExecutableFile(self_spec, true);
  Args &self_args = launch_info.GetArguments();
  self_args.AppendArgument(llvm::StringRef("platform"));
  self_args.AppendArgument(llvm::StringRef("--child-platform-fd"));
  self_args.AppendArgument(llvm::to_string(shared_socket.GetSendableFD()));
  if (gdb_port) {
    self_args.AppendArgument(llvm::StringRef("--gdbserver-port"));
    self_args.AppendArgument(llvm::to_string(gdb_port));
  }
  if (port_offset > 0) {
    self_args.AppendArgument(llvm::StringRef("--port-offset"));
    self_args.AppendArgument(llvm::to_string(port_offset));
  }
  if (!log_file.empty()) {
    self_args.AppendArgument(llvm::StringRef("--log-file"));
    self_args.AppendArgument(log_file);
  }
  if (!log_channels.empty()) {
    self_args.AppendArgument(llvm::StringRef("--log-channels"));
    self_args.AppendArgument(log_channels);
  }
  if (args.GetArgumentCount() > 0) {
    self_args.AppendArgument("--");
    self_args.AppendArguments(args);
  }

  launch_info.SetLaunchInSeparateProcessGroup(false);
  launch_info.SetMonitorProcessCallback(&spawn_process_reaped);

  // Copy the current environment.
  launch_info.GetEnvironment() = Host::GetEnvironment();

  launch_info.GetFlags().Set(eLaunchFlagDisableSTDIO);

  // Close STDIN, STDOUT and STDERR.
  launch_info.AppendCloseFileAction(STDIN_FILENO);
  launch_info.AppendCloseFileAction(STDOUT_FILENO);
  launch_info.AppendCloseFileAction(STDERR_FILENO);

  // Redirect STDIN, STDOUT and STDERR to "/dev/null".
  launch_info.AppendSuppressFileAction(STDIN_FILENO, true, false);
  launch_info.AppendSuppressFileAction(STDOUT_FILENO, false, true);
  launch_info.AppendSuppressFileAction(STDERR_FILENO, false, true);

  std::string cmd;
  self_args.GetCommandString(cmd);

  error = Host::LaunchProcess(launch_info);
  if (error.Fail())
    return error;

  lldb::pid_t child_pid = launch_info.GetProcessID();
  if (child_pid == LLDB_INVALID_PROCESS_ID)
    return Status("invalid pid");

  LLDB_LOG(GetLog(LLDBLog::Platform), "lldb-platform launched '{0}', pid={1}",
           cmd, child_pid);

  {
    std::lock_guard<std::mutex> guard(gdbserver_portmap_mutex);
    gdbserver_portmap.AssociatePortWithProcess(gdb_port, child_pid);
  }

  error = shared_socket.CompleteSending(child_pid);
  if (error.Fail()) {
    Host::Kill(child_pid, SIGTERM);
    return error;
  }

  return Status();
}

// main
int main_platform(int argc, char *argv[]) {
  const char *progname = argv[0];
  const char *subcommand = argv[1];
  argc--;
  argv++;
#if !defined(_WIN32)
  signal(SIGPIPE, SIG_IGN);
  signal(SIGHUP, signal_handler);
#endif
  int long_option_index = 0;
  Status error;
  std::string listen_host_port;
  int ch;

  std::string log_file;
  StringRef
      log_channels; // e.g. "lldb process threads:gdb-remote default:linux all"

  shared_fd_t fd = SharedSocket::kInvalidFD;

  int min_gdbserver_port = 0;
  int max_gdbserver_port = 0;
  uint16_t port_offset = 0;

  FileSpec socket_file;
  bool show_usage = false;
  int option_error = 0;
  int socket_error = -1;

  std::string short_options(OptionParser::GetShortOptionString(g_long_options));

#if __GLIBC__
  optind = 0;
#else
  optreset = 1;
  optind = 1;
#endif

  while ((ch = getopt_long_only(argc, argv, short_options.c_str(),
                                g_long_options, &long_option_index)) != -1) {
    switch (ch) {
    case 0: // Any optional that auto set themselves will return 0
      break;

    case 'L':
      listen_host_port.append(optarg);
      break;

    case 'l': // Set Log File
      if (optarg && optarg[0])
        log_file.assign(optarg);
      break;

    case 'c': // Log Channels
      if (optarg && optarg[0])
        log_channels = StringRef(optarg);
      break;

    case 'f': // Socket file
      if (optarg && optarg[0])
        socket_file.SetFile(optarg, FileSpec::Style::native);
      break;

    case 'p': {
      if (!llvm::to_integer(optarg, port_offset)) {
        WithColor::error() << "invalid port offset string " << optarg << "\n";
        option_error = 4;
        break;
      }
      if (port_offset < LOW_PORT || port_offset > HIGH_PORT) {
        WithColor::error() << llvm::formatv(
            "port offset {0} is not in the "
            "valid user port range of {1} - {2}\n",
            port_offset, LOW_PORT, HIGH_PORT);
        option_error = 5;
      }
    } break;

    case 'P':
    case 'm':
    case 'M': {
      uint16_t portnum;
      if (!llvm::to_integer(optarg, portnum)) {
        WithColor::error() << "invalid port number string " << optarg << "\n";
        option_error = 2;
        break;
      }
      if (portnum < LOW_PORT || portnum > HIGH_PORT) {
        WithColor::error() << llvm::formatv(
            "port number {0} is not in the "
            "valid user port range of {1} - {2}\n",
            portnum, LOW_PORT, HIGH_PORT);
        option_error = 1;
        break;
      }
      if (ch == 'P')
        gdbserver_portmap.AllowPort(portnum);
      else if (ch == 'm')
        min_gdbserver_port = portnum;
      else
        max_gdbserver_port = portnum;
    } break;

    case 2: {
      uint64_t _fd;
      if (!llvm::to_integer(optarg, _fd)) {
        WithColor::error() << "invalid fd " << optarg << "\n";
        option_error = 6;
      } else
        fd = (shared_fd_t)_fd;
    } break;

    case 'h': /* fall-through is intentional */
    case '?':
      show_usage = true;
      break;
    }
  }

  if (!LLDBServerUtilities::SetupLogging(log_file, log_channels, 0))
    return -1;

  // Make a port map for a port range that was specified.
  if (min_gdbserver_port && min_gdbserver_port < max_gdbserver_port) {
    gdbserver_portmap = GDBRemoteCommunicationServerPlatform::PortMap(
        min_gdbserver_port, max_gdbserver_port);
  } else if (min_gdbserver_port || max_gdbserver_port) {
    WithColor::error() << llvm::formatv(
        "--min-gdbserver-port ({0}) is not lower than "
        "--max-gdbserver-port ({1})\n",
        min_gdbserver_port, max_gdbserver_port);
    option_error = 3;
  }

  // Print usage and exit if no listening port is specified.
  if (listen_host_port.empty() && fd == SharedSocket::kInvalidFD)
    show_usage = true;

  if (show_usage || option_error) {
    display_usage(progname, subcommand);
    exit(option_error);
  }

  // Skip any options we consumed with getopt_long_only.
  argc -= optind;
  argv += optind;
  lldb_private::Args inferior_arguments;
  inferior_arguments.SetArguments(argc, const_cast<const char **>(argv));

  if (fd != SharedSocket::kInvalidFD) {
    // Child process will handle the connection and exit.
    Log *log = GetLog(LLDBLog::Platform);
    if (!listen_host_port.empty()) {
      LLDB_LOGF(log, "lldb-platform child: "
                     "ambiguous parameters --listen and --child-platform-fd");
      return socket_error;
    }

    NativeSocket socket;
    error = SharedSocket::GetNativeSocket(fd, socket);
    if (error.Fail()) {
      LLDB_LOGF(log, "lldb-platform child: %s", error.AsCString());
      return socket_error;
    }

    GDBRemoteCommunicationServerPlatform platform(Socket::ProtocolTcp, "tcp");
    if (port_offset > 0)
      platform.SetPortOffset(port_offset);
    platform.SetPortMap(std::move(gdbserver_portmap));
    platform.SetConnection(
        std::unique_ptr<Connection>(new ConnectionFileDescriptor(
            new TCPSocket(socket, /*should_close=*/true,
                          /*child_processes_inherit=*/false))));
    client_handle(platform, inferior_arguments);
    return 0;
  }

  const bool children_inherit_listen_socket = false;
  // the test suite makes many connections in parallel, let's not miss any.
  // The highest this should get reasonably is a function of the number
  // of target CPUs. For now, let's just use 100.
  const int backlog = 100;

  std::unique_ptr<Acceptor> acceptor_up(Acceptor::Create(
      listen_host_port, children_inherit_listen_socket, error));
  if (error.Fail()) {
    fprintf(stderr, "failed to create acceptor: %s", error.AsCString());
    exit(socket_error);
  }

  error = acceptor_up->Listen(backlog);
  if (error.Fail()) {
    printf("failed to listen: %s\n", error.AsCString());
    exit(socket_error);
  }
  if (socket_file) {
    error =
        save_socket_id_to_file(acceptor_up->GetLocalSocketId(), socket_file);
    if (error.Fail()) {
      fprintf(stderr, "failed to write socket id to %s: %s\n",
              socket_file.GetPath().c_str(), error.AsCString());
      return 1;
    }
  }

  GDBRemoteCommunicationServerPlatform platform(
      acceptor_up->GetSocketProtocol(), acceptor_up->GetSocketScheme());
  if (port_offset > 0)
    platform.SetPortOffset(port_offset);

  do {
    const bool children_inherit_accept_socket = true;
    Connection *conn = nullptr;
    error = acceptor_up->Accept(children_inherit_accept_socket, conn);
    if (error.Fail()) {
      WithColor::error() << error.AsCString() << '\n';
      exit(socket_error);
    }
    printf("Connection established.\n");

    if (g_server) {
      std::optional<uint16_t> available_port;
      {
        std::lock_guard<std::mutex> guard(gdbserver_portmap_mutex);
        auto port = gdbserver_portmap.GetNextAvailablePort();
        if (port)
          available_port = *port;
        else
          llvm::consumeError(port.takeError());
      }
      if (!available_port) {
        fprintf(stderr,
                "no available gdbserver port for connection - dropping...\n");
      } else {
        const Socket *conn_socket =
            static_cast<const Socket *>(conn->GetReadObject().get());
        error =
            spawn_process(progname, conn_socket, *available_port, port_offset,
                          inferior_arguments, log_file, log_channels);
        if (error.Fail()) {
          {

            std::lock_guard<std::mutex> guard(gdbserver_portmap_mutex);
            gdbserver_portmap.FreePort(*available_port);
          }
          LLDB_LOGF(GetLog(LLDBLog::Platform), "spawn_process failed: %s",
                    error.AsCString());
          WithColor::error()
              << "spawn_process failed: " << error.AsCString() << "\n";
        }
      }
      // Parent doesn't need a connection to the lldb client
      delete conn;

      // Parent will continue to listen for new connections.
      continue;
    } else {
      // If not running as a server, this process will not accept
      // connections while a connection is active.
      acceptor_up.reset();

      // When not running in server mode, use all available ports
      platform.SetPortMap(std::move(gdbserver_portmap));
    }

    platform.SetConnection(std::unique_ptr<Connection>(conn));
    client_handle(platform, inferior_arguments);
  } while (g_server);

  fprintf(stderr, "lldb-server exiting...\n");

  return 0;
}
