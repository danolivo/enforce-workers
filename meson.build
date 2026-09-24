# contrib/enforce_workers/meson.build

enforce_workers_sources = files(
  'enforce_workers.c',
  'nlguard.c',
  'seqguard.c',
  'idxdefer.c',
)

if host_system == 'windows'
  enforce_workers_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'enforce_workers',
    '--FILEDESC', 'enforce_workers - planner overrides for parallelism and nested loops',])
endif

enforce_workers = shared_module('enforce_workers',
  enforce_workers_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += enforce_workers

install_data(
  'enforce_workers.control',
  'enforce_workers--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'enforce_workers',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'nlguard',
      'seqguard',
      'idxdefer',
    ],
  },
}
