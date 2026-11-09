"""The game a tool works on: game.json in the game's repository (e.g. mk8dx/mk8recomp/game.json).

The file is found through NX_GAME (its path), else in the current directory or one of its parents, so the tools
run from anywhere inside a game's repository. Paths in it are relative to its own directory.

    {
      "name": "mk8recomp",                      executable and project name
      "title": "Mario Kart 8 Deluxe",           window title
      "title_id": "0100152000022000",
      "modules": [["main", "0x7100000000"], ...],   load order = symbol lookup order, with fixed base addresses
      "dump": "../dump_v21",                    exefs/, romfs/, elf/<module>.elf
      "analysis": "../build/analysis_305",      <module>_ida.json from ida_export.py
      "build": "../build"                       generated files: lifted/, image/, shaders/, a64test/, ...
    }
"""
import json
import os


def _find():
    p = os.environ.get('NX_GAME')
    if p:
        return os.path.abspath(p)
    d = os.getcwd()
    while True:
        p = os.path.join(d, 'game.json')
        if os.path.exists(p):
            return p
        up = os.path.dirname(d)
        if up == d:
            raise SystemExit('no game.json: run inside a game repository (e.g. mk8dx/mk8recomp) or set NX_GAME')
        d = up


PATH = _find()
DIR = os.path.dirname(PATH)
_cfg = json.load(open(PATH))


def _path(key):
    return os.path.normpath(os.path.join(DIR, _cfg[key]))


NAME = _cfg['name']
TITLE = _cfg['title']
TITLE_ID = int(_cfg['title_id'], 16)
MODULES = [m for m, _ in _cfg['modules']]
BASES = {m: int(b, 16) for m, b in _cfg['modules']}
DUMP = _path('dump')
ELF = os.path.join(DUMP, 'elf')
ANALYSIS = _path('analysis')
BUILD = _path('build')
OVERRIDES = os.path.join(DIR, 'overrides.txt')       # the game's own host replacements, if any
