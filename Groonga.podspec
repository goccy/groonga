Pod::Spec.new do |s|

  #  root           = '$(PODS_ROOT)/../../groonga-5.0.0'
  root = '/Users/goccy/Development/github/ramen-mania/groonga'
  s.name         = "Groonga"
  s.version      = "1.0.0"
  s.summary      = "Groonga iOS SDK"
  s.description  = ""
  s.homepage     = "http://groonga.org"
  s.author       = { "MasaakiGoshima" => "masaaki.goshima@mixi.co.jp" }
  s.source       = { :git => "https://github.com/groonga/groonga", :tag => "v5.0.0" }
  s.requires_arc = true
  s.public_header_files = "include/**/*.h"
  s.source_files = "config.h"
  header_search_path = "#{root} #{root}/include #{root}/include/groonga #{root}/lib"
  s.prefix_header_contents =<<PREFIX_HEADER_CONTENTS
#define HAVE_STDLIB_H
#define HAVE_SYS_TYPES_H
#define HAVE_SYS_PARAM_H
#define HAVE_SYS_MMAN_H
#define HAVE_SYS_TIME_H
#define HAVE_SYS_RESOURCE_H
#define HAVE_SYS_SOCKET_H
#define HAVE_NETINET_IN_H
#define HAVE_NETINET_TCP_H
#define HAVE_SIGNAL_H
#define HAVE_EXECINFO_H
#define HAVE_NETDB_H
#define HAVE_ERRNO_H
#define HAVE_DLFCN_H


#define HAVE_OPEN
#define HAVE_CLOSE
#define HAVE_READ
#define HAVE_WRITE
#define HAVE_UNISTD_H
#define HAVE_PTHREAD_H
#define HAVE_INTTYPES_H
#define GRN_STACK_SIZE 1024


#define CONFIGURE_OPTIONS ""
#define GRN_CONFIG_PATH "/usr/local/etc/groonga/groonga.conf"
#define GRN_DEFAULT_DB_KEY "auto"
#define GRN_DEFAULT_ENCODING "utf8"
#define GRN_LOCK_TIMEOUT 10000000
#define GRN_LOCK_WAIT_TIME_NANOSECOND 1000000
#define GRN_PLUGIN_SUFFIX ".so"
#define GRN_QUERY_EXPANDER_TSV_RELATIVE_SYNONYMS_FILE "synonyms.tsv"
#define GRN_QUERY_EXPANDER_TSV_SYNONYMS_FILE "NONE/etc/groonga/synonyms.tsv"
#define GRN_VERSION "5.0.0"
#define GRN_DEFAULT_MATCH_ESCALATION_THRESHOLD 0
#define GRN_WITH_NFKC 1
#define GRN_WITH_ZLIB 1
#define HAVE_BACKTRACE 1
#define GRN_LOG_PATH ""
#define GRN_PLUGINS_DIR ""
#define PACKAGE "groonga"

#ifdef DEBUG
#undef DEBUG
#endif

PREFIX_HEADER_CONTENTS
  s.xcconfig = {
    HEADER_SEARCH_PATHS:  header_search_path,
  }

  s.subspec 'include' do |inc|
    inc.source_files = "include/*.h"
    inc.subspec 'groonga' do |groonga|
      groonga.source_files = "include/groonga/*.h"
    end
  end
  s.subspec 'lib' do |lib|
    lib.source_files = "lib/*.{c,cpp}", "lib/ctx.c", "lib/hash.c", "lib/str.c", "lib/db.c", "lib/com.c", "lib/command.c", "lib/error.c", "lib/ii.c", "lib/io.c", "lib/output.c", "lib/*.h"
    lib.exclude_files = "lib/grn_ecmascript.c", "lib/icudump.c"
    
#    lib.subspec 'mrb' do |mrb|
      #mrb.source_files = "lib/mrb/*.{h,c}"
#    end
    lib.subspec 'dat' do |dat|
      dat.source_files = "lib/dat/*.{hpp,cpp}"
    end
  end
end
