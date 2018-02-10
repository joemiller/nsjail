/*

   nsjail - config parsing
   -----------------------------------------

   Copyright 2017 Google Inc. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <fstream>
#include <string>
#include <vector>

#include "caps.h"
#include "cmdline.h"
#include "config.h"
#include "config.pb.h"
#include "logs.h"
#include "macros.h"
#include "mnt.h"
#include "user.h"
#include "util.h"

namespace config {

#define VAL_IF_SET_OR_NULL(njc, val) (njc.has_##val() ? njc.val().c_str() : NULL)

static uint64_t configRLimit(
    int res, const nsjail::RLimit& rl, const uint64_t val, unsigned long mul = 1UL) {
	if (rl == nsjail::RLimit::VALUE) {
		return (val * mul);
	}
	if (rl == nsjail::RLimit::SOFT) {
		return cmdline::parseRLimit(res, "soft", mul);
	}
	if (rl == nsjail::RLimit::HARD) {
		return cmdline::parseRLimit(res, "hard", mul);
	}
	if (rl == nsjail::RLimit::INF) {
		return RLIM64_INFINITY;
	}
	LOG_F("Unknown rlimit value type for rlimit:%d", res);
	abort();
}

static bool configParseInternal(nsjconf_t* nsjconf, const nsjail::NsJailConfig& njc) {
	switch (njc.mode()) {
	case nsjail::Mode::LISTEN:
		nsjconf->mode = MODE_LISTEN_TCP;
		break;
	case nsjail::Mode::ONCE:
		nsjconf->mode = MODE_STANDALONE_ONCE;
		break;
	case nsjail::Mode::RERUN:
		nsjconf->mode = MODE_STANDALONE_RERUN;
		break;
	case nsjail::Mode::EXECVE:
		nsjconf->mode = MODE_STANDALONE_EXECVE;
		break;
	default:
		LOG_E("Uknown running mode: %d", njc.mode());
		return false;
	}
	if (njc.has_chroot_dir()) {
		nsjconf->chroot = njc.chroot_dir();
	}
	nsjconf->is_root_rw = njc.is_root_rw();
	nsjconf->hostname = njc.hostname();
	nsjconf->cwd = njc.cwd();
	nsjconf->port = njc.port();
	nsjconf->bindhost = njc.bindhost().c_str();
	nsjconf->max_conns_per_ip = njc.max_conns_per_ip();
	nsjconf->tlimit = njc.time_limit();
	nsjconf->max_cpus = njc.max_cpus();
	nsjconf->daemonize = njc.daemon();

	if (njc.has_log_fd()) {
		nsjconf->logfile = "/dev/fd/" + std::to_string(njc.log_fd());
	}
	if (njc.has_log_file()) {
		nsjconf->logfile = njc.log_file();
	}
	if (njc.has_log_level()) {
		switch (njc.log_level()) {
		case nsjail::LogLevel::DEBUG:
			nsjconf->loglevel = logs::DEBUG;
			break;
		case nsjail::LogLevel::INFO:
			nsjconf->loglevel = logs::INFO;
			break;
		case nsjail::LogLevel::WARNING:
			nsjconf->loglevel = logs::WARNING;
			break;
		case nsjail::LogLevel::ERROR:
			nsjconf->loglevel = logs::ERROR;
			break;
		case nsjail::LogLevel::FATAL:
			nsjconf->loglevel = logs::FATAL;
			break;
		default:
			LOG_E("Unknown log_level: %d", njc.log_level());
			return false;
		}
	}

	nsjconf->keep_env = njc.keep_env();
	for (ssize_t i = 0; i < njc.envar_size(); i++) {
		nsjconf->envs.push_back(njc.envar(i));
	}

	nsjconf->keep_caps = njc.keep_caps();
	for (ssize_t i = 0; i < njc.cap_size(); i++) {
		int cap = caps::nameToVal(njc.cap(i).c_str());
		if (cap == -1) {
			return false;
		}
		nsjconf->caps.push_back(cap);
	}

	nsjconf->is_silent = njc.silent();
	nsjconf->skip_setsid = njc.skip_setsid();

	for (ssize_t i = 0; i < njc.pass_fd_size(); i++) {
		nsjconf->openfds.push_back(i);
	}

	nsjconf->disable_no_new_privs = njc.disable_no_new_privs();

	nsjconf->rl_as =
	    configRLimit(RLIMIT_AS, njc.rlimit_as_type(), njc.rlimit_as(), 1024UL * 1024UL);
	nsjconf->rl_core =
	    configRLimit(RLIMIT_CORE, njc.rlimit_core_type(), njc.rlimit_core(), 1024UL * 1024UL);
	nsjconf->rl_cpu = configRLimit(RLIMIT_CPU, njc.rlimit_cpu_type(), njc.rlimit_cpu());
	nsjconf->rl_fsize = configRLimit(
	    RLIMIT_FSIZE, njc.rlimit_fsize_type(), njc.rlimit_fsize(), 1024UL * 1024UL);
	nsjconf->rl_nofile =
	    configRLimit(RLIMIT_NOFILE, njc.rlimit_nofile_type(), njc.rlimit_nofile());
	nsjconf->rl_nproc = configRLimit(RLIMIT_NPROC, njc.rlimit_nproc_type(), njc.rlimit_nproc());
	nsjconf->rl_stack = configRLimit(
	    RLIMIT_STACK, njc.rlimit_stack_type(), njc.rlimit_stack(), 1024UL * 1024UL);

	if (njc.persona_addr_compat_layout()) {
		nsjconf->personality |= ADDR_COMPAT_LAYOUT;
	}
	if (njc.persona_mmap_page_zero()) {
		nsjconf->personality |= MMAP_PAGE_ZERO;
	}
	if (njc.persona_read_implies_exec()) {
		nsjconf->personality |= READ_IMPLIES_EXEC;
	}
	if (njc.persona_addr_limit_3gb()) {
		nsjconf->personality |= ADDR_LIMIT_3GB;
	}
	if (njc.persona_addr_no_randomize()) {
		nsjconf->personality |= ADDR_NO_RANDOMIZE;
	}

	nsjconf->clone_newnet = njc.clone_newnet();
	nsjconf->clone_newuser = njc.clone_newuser();
	nsjconf->clone_newns = njc.clone_newns();
	nsjconf->clone_newpid = njc.clone_newpid();
	nsjconf->clone_newipc = njc.clone_newipc();
	nsjconf->clone_newuts = njc.clone_newuts();
	nsjconf->clone_newcgroup = njc.clone_newcgroup();

	for (ssize_t i = 0; i < njc.uidmap_size(); i++) {
		if (!user::parseId(nsjconf, VAL_IF_SET_OR_NULL(njc.uidmap(i), inside_id),
			VAL_IF_SET_OR_NULL(njc.uidmap(i), outside_id), njc.uidmap(i).count(),
			false /* is_gid */, njc.uidmap(i).use_newidmap())) {
			return false;
		}
	}
	for (ssize_t i = 0; i < njc.gidmap_size(); i++) {
		if (!user::parseId(nsjconf, VAL_IF_SET_OR_NULL(njc.gidmap(i), inside_id),
			VAL_IF_SET_OR_NULL(njc.gidmap(i), outside_id), njc.gidmap(i).count(),
			true /* is_gid */, njc.gidmap(i).use_newidmap())) {
			return false;
		}
	}

	if (!njc.mount_proc()) {
		nsjconf->proc_path.clear();
	}
	for (ssize_t i = 0; i < njc.mount_size(); i++) {
		const char* src = (njc.mount(i).has_src()) ? njc.mount(i).src().c_str() : NULL;
		const char* src_env = (njc.mount(i).has_prefix_src_env())
					  ? njc.mount(i).prefix_src_env().c_str()
					  : NULL;
		const char* dst = (njc.mount(i).has_dst()) ? njc.mount(i).dst().c_str() : NULL;
		const char* dst_env = (njc.mount(i).has_prefix_dst_env())
					  ? njc.mount(i).prefix_dst_env().c_str()
					  : NULL;
		const char* fstype =
		    (njc.mount(i).has_fstype()) ? njc.mount(i).fstype().c_str() : NULL;
		const char* options =
		    (njc.mount(i).has_options()) ? njc.mount(i).options().c_str() : NULL;

		uintptr_t flags = (njc.mount(i).rw() == false) ? MS_RDONLY : 0;
		flags |= njc.mount(i).is_bind() ? (MS_BIND | MS_REC | MS_PRIVATE) : 0;
		bool mandatory = njc.mount(i).mandatory();

		mnt::isDir_t isDir = mnt::NS_DIR_MAYBE;
		if (njc.mount(i).has_is_dir()) {
			isDir = njc.mount(i).is_dir() ? mnt::NS_DIR_YES : mnt::NS_DIR_NO;
		}

		const char* src_content = NULL;
		size_t src_content_len = 0;
		if (njc.mount(i).has_src_content()) {
			src_content = njc.mount(i).src_content().data();
			src_content_len = njc.mount(i).src_content().size();
		}

		if (mnt::addMountPtTail(nsjconf, src, dst, fstype, options, flags, isDir, mandatory,
			src_env, dst_env, src_content, src_content_len,
			njc.mount(i).is_symlink()) == false) {
			LOG_E("Couldn't add mountpoint for src:'%s' dst:'%s'", src, dst);
			return false;
		}
	}

	if (njc.has_seccomp_policy_file()) {
		nsjconf->kafel_file_path = njc.seccomp_policy_file();
	}
	for (ssize_t i = 0; i < njc.seccomp_string().size(); i++) {
		nsjconf->kafel_string += njc.seccomp_string(i);
		nsjconf->kafel_string += '\n';
	}

	nsjconf->cgroup_mem_max = njc.cgroup_mem_max();
	nsjconf->cgroup_mem_mount = njc.cgroup_mem_mount().c_str();
	nsjconf->cgroup_mem_parent = njc.cgroup_mem_parent().c_str();
	nsjconf->cgroup_pids_max = njc.cgroup_pids_max();
	nsjconf->cgroup_pids_mount = njc.cgroup_pids_mount().c_str();
	nsjconf->cgroup_pids_parent = njc.cgroup_pids_parent().c_str();
	nsjconf->cgroup_net_cls_classid = njc.cgroup_net_cls_classid();
	nsjconf->cgroup_net_cls_mount = njc.cgroup_net_cls_mount().c_str();
	nsjconf->cgroup_net_cls_parent = njc.cgroup_net_cls_parent().c_str();

	nsjconf->iface_lo = !(njc.iface_no_lo());
	if (njc.has_macvlan_iface()) {
		nsjconf->iface_vs = njc.macvlan_iface();
	}
	nsjconf->iface_vs_ip = njc.macvlan_vs_ip().c_str();
	nsjconf->iface_vs_nm = njc.macvlan_vs_nm().c_str();
	nsjconf->iface_vs_gw = njc.macvlan_vs_gw().c_str();

	if (njc.has_exec_bin()) {
		static std::vector<const char*> argv;
		if (njc.exec_bin().has_arg0()) {
			argv.push_back(njc.exec_bin().arg0().c_str());
			nsjconf->exec_file = njc.exec_bin().path().c_str();
		} else {
			argv.push_back(njc.exec_bin().path().c_str());
		}
		for (ssize_t i = 0; i < njc.exec_bin().arg().size(); i++) {
			argv.push_back(njc.exec_bin().arg(i).c_str());
		}
		argv.push_back(nullptr);
		nsjconf->argv = argv.data();
		nsjconf->use_execveat = njc.exec_bin().exec_fd();
	}

	return true;
}

static void LogHandler(
    google::protobuf::LogLevel level, const char* filename, int line, const std::string& message) {
	LOG_W("config.cc: '%s'", message.c_str());
}

bool parseFile(nsjconf_t* nsjconf, const char* file) {
	LOG_I("Parsing configuration from '%s'", file);

	int fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		PLOG_W("Couldn't open config file '%s'", file);
		return false;
	}

	SetLogHandler(LogHandler);
	google::protobuf::io::FileInputStream input(fd);
	input.SetCloseOnDelete(true);

	/* Use static so we can get c_str() pointers, and copy them into the nsjconf struct */
	static nsjail::NsJailConfig nsc;

	auto parser = google::protobuf::TextFormat::Parser();
	if (!parser.Parse(&input, &nsc)) {
		LOG_W("Couldn't parse file '%s' from Text into ProtoBuf", file);
		return false;
	}
	if (!configParseInternal(nsjconf, nsc)) {
		LOG_W("Couldn't parse the ProtoBuf");
		return false;
	}
	LOG_D("Parsed config:\n'%s'", nsc.DebugString().c_str());

	return true;
}

}  // namespace config
