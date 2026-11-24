"""airflash commands for idf.py. Copy this file into your ESP-IDF project's root folder (next to CMakeLists.txt):

    idf.py airflash-flash      build, check, hand over and upload over Wi-Fi
    idf.py airflash-monitor    idf.py monitor over Wi-Fi (crash decoding)
    idf.py airflash-logs       plain log stream
    idf.py airflash-coredump   download + decode the last crash dump
    idf.py airflash-info       what's running on the board
    idf.py airflash-run        updater: boot the installed app (e.g. after the recovery pin)

Board address: --airflash-host <ip>, or the AIRFLASH_HOST environment variable.
(ESP-IDF 5.x only loads idf.py extensions from the project folder, hence the copy.)
"""
import importlib.util
import json
import os


def _airflash_tool(build_dir):
    """Load airflash.py from the airflash component used by this project's last build."""
    try:
        with open(os.path.join(build_dir, 'project_description.json')) as f:
            components = json.load(f)['build_component_info']
    except (OSError, KeyError, ValueError):
        raise SystemExit('airflash: build the project once first (idf.py build)')
    for name, info in components.items():
        if name == 'airflash' or name.endswith('__airflash'):
            spec = importlib.util.spec_from_file_location('airflash_tool', os.path.join(info['dir'], 'tool', 'airflash.py'))
            tool = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(tool)
            return tool
    raise SystemExit('airflash: the airflash component is not part of this project')


def action_extensions(base_actions, project_path):
    def command(name, extra=()):
        def callback(action, ctx, args, airflash_host, airflash_token, **kwargs):
            argv = [name] + list(extra) + ['--host', airflash_host]
            if airflash_token:
                argv += ['--token', airflash_token]
            if name in ('flash', 'monitor', 'coredump'):
                argv.insert(1, args.project_dir)
            if kwargs.get('airflash_force'):
                argv.append('--force')
            code = _airflash_tool(args.build_dir).main(argv)
            if code:
                raise SystemExit(code)
        return callback

    common = [
        {'names': ['--airflash-host'], 'help': 'Board IP or name.', 'envvar': 'AIRFLASH_HOST', 'default': 'airflash.local'},
        {'names': ['--airflash-token'], 'help': 'X-Token, if set on the board.', 'envvar': 'AIRFLASH_TOKEN', 'default': ''},
    ]
    return {
        'actions': {
            'airflash-flash': {
                'callback': command('flash', ['--no-build']),  # idf.py builds first (dependency below)
                'help': 'Flash the app over Wi-Fi via the airflash updater.',
                'dependencies': ['all'],
                'options': common + [{'names': ['--airflash-force'], 'is_flag': True,
                                      'help': 'Flash even if the compatibility checks fail.'}],
            },
            'airflash-monitor': {'callback': command('monitor'), 'help': 'idf.py monitor over Wi-Fi.', 'options': common},
            'airflash-logs': {'callback': command('logs'), 'help': 'Plain log stream over Wi-Fi.', 'options': common},
            'airflash-coredump': {'callback': command('coredump'), 'help': 'Download and decode the last crash dump.', 'options': common},
            'airflash-info': {'callback': command('info'), 'help': "What's running on the board.", 'options': common},
            'airflash-run': {'callback': command('run'), 'help': 'Updater: boot the installed app.', 'options': common},
        }
    }
