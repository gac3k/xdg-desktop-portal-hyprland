sources = files([
                    'main.cpp',
                ])

config_h = configuration_data()
config_h.set('_GNU_SOURCE', '1')
config_h.set_quoted('EI_VERSION', meson.project_version())
config_h.set_quoted('EIS_VERSION', meson.project_version())
if cc.has_function('memfd_create', prefix: '#define _GNU_SOURCE\n#include <sys/mman.h>')
    config_h.set10('HAVE_MEMFD_CREATE', true)
endif

dep_math = cc.find_library('m', required: false)
dep_epoll = dependency('epoll-shim', required: false)
dep_libxkbcommon = dependency('xkbcommon', required: false)
config_h.set10('HAVE_LIBXKBCOMMON', dep_libxkbcommon.found())
dep_libevdev = dependency('libevdev', required: false)
config_h.set10('HAVE_LIBEVDEV', dep_libevdev.found())
inc_builddir = include_directories('.')
inc_src = include_directories('src')

config_h = configuration_data()
config_h.set('_GNU_SOURCE', '1')
config_h.set_quoted('EI_VERSION', meson.project_version())
config_h.set_quoted('EIS_VERSION', meson.project_version())
if cc.has_function('memfd_create', prefix: '#define _GNU_SOURCE\n#include <sys/mman.h>')
    config_h.set10('HAVE_MEMFD_CREATE', true)
endif

dep_math = cc.find_library('m', required: false)
dep_epoll = dependency('epoll-shim', required: false)
dep_libxkbcommon = dependency('xkbcommon', required: false)
config_h.set10('HAVE_LIBXKBCOMMON', dep_libxkbcommon.found())
dep_libevdev = dependency('libevdev', required: false)
config_h.set10('HAVE_LIBEVDEV', dep_libevdev.found())

if not get_option('liboeffis').disabled()
    sd_bus_provider = get_option('sd-bus-provider')
    if sd_bus_provider == 'auto'
        message('Trying to find an sd-bus provider...')
        providers = ['libsystemd', 'libelogind', 'basu']
        foreach provider: providers
            if dependency(provider, required: false).found()
                message('Using sd-bus provider "@0@"'.format(provider))
                sd_bus_provider = provider
                break
            endif
        endforeach
        if sd_bus_provider  == 'auto'
            error('Failed to find an sd-bus provider, tried @0@'.format(', '.join(providers)))
        endif
        summary({'sd-bus-provider': sd_bus_provider}, section: 'Conditional Features')
    endif
    dep_sdbus = dependency(sd_bus_provider, required: get_option('liboeffis'))
else
    dep_sdbus = dependency('', required: false)
endif
config_h.set10('HAVE_LIBSYSTEMD', dep_sdbus.found() and dep_sdbus.name() == 'libsystemd')
config_h.set10('HAVE_LIBELOGIND', dep_sdbus.found() and dep_sdbus.name() == 'libelogind')
config_h.set10('HAVE_BASU', dep_sdbus.found() and dep_sdbus.name() == 'basu')

configure_file(output: 'config.h', configuration: config_h)

executable('hyprland-eis-server',
           sources,
           moc,
           dependencies: [
               dependency('libei-1.0'),
               dependency('xkbcommon'),
           ],
           install: true
)