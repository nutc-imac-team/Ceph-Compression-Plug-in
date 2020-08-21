// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/parsers.hpp>

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include "global/global_init.h"
#include "common/ceph_argparse.h"
#include "include/stringify.h"
#include "common/errno.h"
#include "common/safe_io.h"

#include "os/bluestore/BlueFS.h"
#include "os/bluestore/BlueStore.h"

namespace po = boost::program_options;

void usage(po::options_description &desc)
{
  cout << desc << std::endl;
}

void validate_path(CephContext *cct, const string& path, bool bluefs)
{
  BlueStore bluestore(cct, path);
  string type;
  int r = bluestore.read_meta("type", &type);
  if (r < 0) {
    cerr << "failed to load os-type: " << cpp_strerror(r) << std::endl;
    exit(EXIT_FAILURE);
  }
  if (type != "bluestore") {
    cerr << "expected bluestore, but type is " << type << std::endl;
    exit(EXIT_FAILURE);
  }
  if (!bluefs) {
    return;
  }

  string kv_backend;
  r = bluestore.read_meta("kv_backend", &kv_backend);
  if (r < 0) {
    cerr << "failed to load kv_backend: " << cpp_strerror(r) << std::endl;
    exit(EXIT_FAILURE);
  }
  if (kv_backend != "rocksdb") {
    cerr << "expect kv_backend to be rocksdb, but is " << kv_backend
         << std::endl;
    exit(EXIT_FAILURE);
  }
  string bluefs_enabled;
  r = bluestore.read_meta("bluefs", &bluefs_enabled);
  if (r < 0) {
    cerr << "failed to load do_bluefs: " << cpp_strerror(r) << std::endl;
    exit(EXIT_FAILURE);
  }
  if (bluefs_enabled != "1") {
    cerr << "bluefs not enabled for rocksdb" << std::endl;
    exit(EXIT_FAILURE);
  }
}

const char* find_device_path(
  int id,
  CephContext *cct,
  const vector<string>& devs)
{
  for (auto& i : devs) {
    bluestore_bdev_label_t label;
    int r = BlueStore::_read_bdev_label(cct, i, &label);
    if (r < 0) {
      cerr << "unable to read label for " << i << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
    if ((id == BlueFS::BDEV_SLOW && label.description == "main") ||
        (id == BlueFS::BDEV_DB && label.description == "bluefs db") ||
        (id == BlueFS::BDEV_WAL && label.description == "bluefs wal")) {
      return i.c_str();
    }
  }
  return nullptr;
}

void add_devices(
  BlueFS *fs,
  CephContext *cct,
  const vector<string>& devs)
{
  string main;
  set<int> got;
  for (auto& i : devs) {
    bluestore_bdev_label_t label;
    int r = BlueStore::_read_bdev_label(cct, i, &label);
    if (r < 0) {
      cerr << "unable to read label for " << i << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
    int id = -1;
    if (label.description == "main")
      main = i;
    else if (label.description == "bluefs db")
      id = BlueFS::BDEV_DB;
    else if (label.description == "bluefs wal")
      id = BlueFS::BDEV_WAL;
    if (id >= 0) {
      got.insert(id);
      cout << " slot " << id << " " << i << std::endl;
      int r = fs->add_block_device(id, i, false);
      if (r < 0) {
	cerr << "unable to open " << i << ": " << cpp_strerror(r) << std::endl;
	exit(EXIT_FAILURE);
      }
    }
  }
  if (main.length()) {
    int id = BlueFS::BDEV_DB;
    if (got.count(BlueFS::BDEV_DB))
      id = BlueFS::BDEV_SLOW;
    cout << " slot " << id << " " << main << std::endl;
    int r = fs->add_block_device(id, main, false);
    if (r < 0) {
      cerr << "unable to open " << main << ": " << cpp_strerror(r)
	   << std::endl;
      exit(EXIT_FAILURE);
    }
  }
}

BlueFS *open_bluefs(
  CephContext *cct,
  const string& path,
  const vector<string>& devs)
{
  validate_path(cct, path, true);
  BlueFS *fs = new BlueFS(cct);

  add_devices(fs, cct, devs);

  int r = fs->mount();
  if (r < 0) {
    cerr << "unable to mount bluefs: " << cpp_strerror(r)
	 << std::endl;
    exit(EXIT_FAILURE);
  }
  return fs;
}

void log_dump(
  CephContext *cct,
  const string& path,
  const vector<string>& devs)
{
  BlueFS* fs = open_bluefs(cct, path, devs);
  int r = fs->log_dump();
  if (r < 0) {
    cerr << "log_dump failed" << ": "
         << cpp_strerror(r) << std::endl;
    exit(EXIT_FAILURE);
  }

  delete fs;
}

void inferring_bluefs_devices(vector<string>& devs, std::string& path)
{
  cout << "inferring bluefs devices from bluestore path" << std::endl;
  for (auto fn : {"block", "block.wal", "block.db"}) {
    string p = path + "/" + fn;
    struct stat st;
    if (::stat(p.c_str(), &st) == 0) {
      devs.push_back(p);
    }
  }
}

int main(int argc, char **argv)
{
  string out_dir;
  vector<string> devs;
  string path;
  string action;
  string log_file;
  string key, value;
  int log_level = 30;
  bool fsck_deep = false;
  po::options_description po_options("Options");
  po_options.add_options()
    ("help,h", "produce help message")
    ("path", po::value<string>(&path), "bluestore path")
    ("out-dir", po::value<string>(&out_dir), "output directory")
    ("log-file,l", po::value<string>(&log_file), "log file")
    ("log-level", po::value<int>(&log_level), "log level (30=most, 20=lots, 10=some, 1=little)")
    ("dev", po::value<vector<string>>(&devs), "device(s)")
    ("deep", po::value<bool>(&fsck_deep), "deep fsck (read all data)")
    ("key,k", po::value<string>(&key), "label metadata key name")
    ("value,v", po::value<string>(&value), "label metadata value")
    ;
  po::options_description po_positional("Positional options");
  po_positional.add_options()
    ("command", po::value<string>(&action), "fsck, repair, bluefs-export, bluefs-bdev-sizes, bluefs-bdev-expand, show-label, set-label-key, rm-label-key, prime-osd-dir, bluefs-log-dump")
    ;
  po::options_description po_all("All options");
  po_all.add(po_options).add(po_positional);
  po::positional_options_description pd;
  pd.add("command", 1);

  vector<string> ceph_option_strings;
  po::variables_map vm;
  try {
    po::parsed_options parsed =
      po::command_line_parser(argc, argv).options(po_all).allow_unregistered().positional(pd).run();
    po::store( parsed, vm);
    po::notify(vm);
    ceph_option_strings = po::collect_unrecognized(parsed.options,
						   po::include_positional);
  } catch(po::error &e) {
    std::cerr << e.what() << std::endl;
    exit(EXIT_FAILURE);
  }

  if (vm.count("help")) {
    usage(po_all);
    exit(EXIT_SUCCESS);
  }
  if (action.empty()) {
    cerr << "must specify an action; --help for help" << std::endl;
    exit(EXIT_FAILURE);
  }

  if (action == "fsck" || action == "repair") {
    if (path.empty()) {
      cerr << "must specify bluestore path" << std::endl;
      exit(EXIT_FAILURE);
    }
  }
  if (action == "prime-osd-dir") {
    if (devs.size() != 1) {
      cerr << "must specify the main bluestore device" << std::endl;
      exit(EXIT_FAILURE);
    }
    if (path.empty()) {
      cerr << "must specify osd dir to prime" << std::endl;
      exit(EXIT_FAILURE);
    }
  }
  if (action == "set-label-key" ||
      action == "rm-label-key") {
    if (devs.size() != 1) {
      cerr << "must specify the main bluestore device" << std::endl;
      exit(EXIT_FAILURE);
    }
    if (key.size() == 0) {
      cerr << "must specify a key name with -k" << std::endl;
      exit(EXIT_FAILURE);
    }
    if (action == "set-label-key" && value.size() == 0) {
      cerr << "must specify a value with -v" << std::endl;
      exit(EXIT_FAILURE);
    }
  }
  if (action == "show-label") {
    if (devs.empty() && path.empty()) {
      cerr << "must specify bluestore path *or* raw device(s)" << std::endl;
      exit(EXIT_FAILURE);
    }
    if (devs.empty())
      inferring_bluefs_devices(devs, path);
  }
  if (action == "bluefs-export" || action == "bluefs-log-dump") {
    if (path.empty()) {
      cerr << "must specify bluestore path" << std::endl;
      exit(EXIT_FAILURE);
    }
    if ((action == "bluefs-export") && out_dir.empty()) {
      cerr << "must specify out-dir to export bluefs" << std::endl;
      exit(EXIT_FAILURE);
    }
    inferring_bluefs_devices(devs, path);
  }
  if (action == "bluefs-bdev-sizes" || action == "bluefs-bdev-expand") {
    if (path.empty()) {
      cerr << "must specify bluestore path" << std::endl;
      exit(EXIT_FAILURE);
    }
    inferring_bluefs_devices(devs, path);
  }

  vector<const char*> args;
  if (log_file.size()) {
    args.push_back("--log-file");
    args.push_back(log_file.c_str());
    static char ll[10];
    snprintf(ll, sizeof(ll), "%d", log_level);
    args.push_back("--debug-bluestore");
    args.push_back(ll);
    args.push_back("--debug-bluefs");
    args.push_back(ll);
  }
  args.push_back("--no-log-to-stderr");
  args.push_back("--err-to-stderr");

  for (auto& i : ceph_option_strings) {
    args.push_back(i.c_str());
  }
  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);

  common_init_finish(cct.get());

  if (action == "fsck" ||
      action == "repair") {
    validate_path(cct.get(), path, false);
    BlueStore bluestore(cct.get(), path);
    int r;
    if (action == "fsck") {
      r = bluestore.fsck(fsck_deep);
    } else {
      r = bluestore.repair(fsck_deep);
    }
    if (r < 0) {
      cerr << "error from fsck: " << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    } else if (r > 0) {
      cerr << action << " found " << r << " error(s)" << std::endl;
      exit(EXIT_FAILURE);
    } else {
      cout << action << " success" << std::endl;
    }
  }
  else if (action == "prime-osd-dir") {
    bluestore_bdev_label_t label;
    int r = BlueStore::_read_bdev_label(cct.get(), devs.front(), &label);
    if (r < 0) {
      cerr << "failed to read label for " << devs.front() << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }

    // kludge some things into the map that we want to populate into
    // target dir
    label.meta["path_block"] = devs.front();
    label.meta["type"] = "bluestore";
    label.meta["fsid"] = stringify(label.osd_uuid);
    
    for (auto kk : {
	"whoami",
	  "osd_key",
	  "ceph_fsid",
	  "fsid",
	  "type",
	  "ready" }) {
      string k = kk;
      auto i = label.meta.find(k);
      if (i == label.meta.end()) {
	continue;
      }
      string p = path + "/" + k;
      string v = i->second;
      if (k == "osd_key") {
	p = path + "/keyring";
	v = "[osd.";
	v += label.meta["whoami"];
	v += "]\nkey = " + i->second;
      }
      v += "\n";
      int fd = ::open(p.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0600);
      if (fd < 0) {
	cerr << "error writing " << p << ": " << cpp_strerror(errno)
	     << std::endl;
	exit(EXIT_FAILURE);
      }
      int r = safe_write(fd, v.c_str(), v.size());
      if (r < 0) {
	cerr << "error writing to " << p << ": " << cpp_strerror(errno)
	     << std::endl;
	exit(EXIT_FAILURE);
      }
      ::close(fd);
    }
  }
  else if (action == "show-label") {
    JSONFormatter jf(true);
    jf.open_object_section("devices");
    for (auto& i : devs) {
      bluestore_bdev_label_t label;
      int r = BlueStore::_read_bdev_label(cct.get(), i, &label);
      if (r < 0) {
	cerr << "unable to read label for " << i << ": "
	     << cpp_strerror(r) << std::endl;
	exit(EXIT_FAILURE);
      }
      jf.open_object_section(i.c_str());
      label.dump(&jf);
      jf.close_section();
    }
    jf.close_section();
    jf.flush(cout);
  }
  else if (action == "set-label-key") {
    bluestore_bdev_label_t label;
    int r = BlueStore::_read_bdev_label(cct.get(), devs.front(), &label);
    if (r < 0) {
      cerr << "unable to read label for " << devs.front() << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
    if (key == "size") {
      label.size = strtoull(value.c_str(), nullptr, 10);
    } else if (key =="osd_uuid") {
      label.osd_uuid.parse(value.c_str());
    } else if (key =="btime") {
      uint64_t epoch;
      uint64_t nsec;
      int r = utime_t::parse_date(value.c_str(), &epoch, &nsec);
      if (r == 0) {
	label.btime = utime_t(epoch, nsec);
      }
    } else if (key =="description") {
      label.description = value;
    } else {
      label.meta[key] = value;
    }
    r = BlueStore::_write_bdev_label(cct.get(), devs.front(), label);
    if (r < 0) {
      cerr << "unable to write label for " << devs.front() << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
  }
  else if (action == "rm-label-key") {
    bluestore_bdev_label_t label;
    int r = BlueStore::_read_bdev_label(cct.get(), devs.front(), &label);
    if (r < 0) {
      cerr << "unable to read label for " << devs.front() << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
    if (!label.meta.count(key)) {
      cerr << "key '" << key << "' not present" << std::endl;
      exit(EXIT_FAILURE);
    }
    label.meta.erase(key);
    r = BlueStore::_write_bdev_label(cct.get(), devs.front(), label);
    if (r < 0) {
      cerr << "unable to write label for " << devs.front() << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
  }
  else if (action == "bluefs-bdev-sizes") {
    BlueFS *fs = open_bluefs(cct.get(), path, devs);
    fs->dump_block_extents(cout);
    delete fs;
  }
  else if (action == "bluefs-bdev-expand") {
    BlueFS *fs = open_bluefs(cct.get(), path, devs);
    cout << "start:" << std::endl;
    fs->dump_block_extents(cout);
    for (int devid : { BlueFS::BDEV_WAL, BlueFS::BDEV_DB }) {
      interval_set<uint64_t> before;
      fs->get_block_extents(devid, &before);
      if (before.empty()) continue;
      uint64_t end = before.range_end();
      uint64_t size = fs->get_block_device_size(devid);
      if (end < size) {
	cout << "expanding dev " << devid << " from 0x" << std::hex
	     << end << " to 0x" << size << std::dec << std::endl;
	fs->add_block_extent(devid, end, size-end);
	const char* path = find_device_path(devid, cct.get(), devs);
	if (path == nullptr) {
	  cerr << "Can't find device path for dev " << devid << std::endl;
	  continue;
	}
	bluestore_bdev_label_t label;
	int r = BlueStore::_read_bdev_label(cct.get(), path, &label);
	if (r < 0) {
	  cerr << "unable to read label for " << path << ": "
		<< cpp_strerror(r) << std::endl;
	  continue;
	}
        label.size = size;
	r = BlueStore::_write_bdev_label(cct.get(), path, label);
	if (r < 0) {
	  cerr << "unable to write label for " << path << ": "
		<< cpp_strerror(r) << std::endl;
	  continue;
	}
	cout << "dev " << devid << " size label updated to "
	      << size << std::endl;
      }
    }
    delete fs;
  }
  else if (action == "bluefs-export") {
    BlueFS *fs = open_bluefs(cct.get(), path, devs);

    vector<string> dirs;
    int r = fs->readdir("", &dirs);
    if (r < 0) {
      cerr << "readdir in root failed: " << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }

    if (::access(out_dir.c_str(), F_OK)) {
      r = ::mkdir(out_dir.c_str(), 0755);
      if (r < 0) {
        r = -errno;
        cerr << "mkdir " << out_dir << " failed: " << cpp_strerror(r) << std::endl;
        exit(EXIT_FAILURE);
      }
    }

    for (auto& dir : dirs) {
      if (dir[0] == '.')
	continue;
      cout << dir << "/" << std::endl;
      vector<string> ls;
      r = fs->readdir(dir, &ls);
      if (r < 0) {
	cerr << "readdir " << dir << " failed: " << cpp_strerror(r) << std::endl;
	exit(EXIT_FAILURE);
      }
      string full = out_dir + "/" + dir;
      if (::access(full.c_str(), F_OK)) {
        r = ::mkdir(full.c_str(), 0755);
        if (r < 0) {
          r = -errno;
          cerr << "mkdir " << full << " failed: " << cpp_strerror(r) << std::endl;
          exit(EXIT_FAILURE);
        }
      }
      for (auto& file : ls) {
	if (file[0] == '.')
	  continue;
	cout << dir << "/" << file << std::endl;
	uint64_t size;
	utime_t mtime;
	r = fs->stat(dir, file, &size, &mtime);
	if (r < 0) {
	  cerr << "stat " << file << " failed: " << cpp_strerror(r) << std::endl;
	  exit(EXIT_FAILURE);
	}
	string path = out_dir + "/" + dir + "/" + file;
	int fd = ::open(path.c_str(), O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC, 0644);
	if (fd < 0) {
	  r = -errno;
	  cerr << "open " << path << " failed: " << cpp_strerror(r) << std::endl;
	  exit(EXIT_FAILURE);
	}
	if (size > 0) {
	  BlueFS::FileReader *h;
	  r = fs->open_for_read(dir, file, &h, false);
	  if (r < 0) {
	    cerr << "open_for_read " << dir << "/" << file << " failed: "
		 << cpp_strerror(r) << std::endl;
	    exit(EXIT_FAILURE);
	  }
	  int pos = 0;
	  int left = size;
	  while (left) {
	    bufferlist bl;
	    r = fs->read(h, &h->buf, pos, left, &bl, NULL);
	    if (r <= 0) {
	      cerr << "read " << dir << "/" << file << " from " << pos
		   << " failed: " << cpp_strerror(r) << std::endl;
	      exit(EXIT_FAILURE);
	    }
	    int rc = bl.write_fd(fd);
	    if (rc < 0) {
	      cerr << "write to " << path << " failed: "
		   << cpp_strerror(r) << std::endl;
	      exit(EXIT_FAILURE);
	    }
	    pos += r;
	    left -= r;
	  }
	  delete h;
	}
	::close(fd);
      }
    }
    fs->umount();
    delete fs;
  } else if (action == "bluefs-log-dump") {
    log_dump(cct.get(), path, devs);
  } else {
    cerr << "unrecognized action " << action << std::endl;
    return 1;
  }

  return 0;
}