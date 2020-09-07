#!/usr/bin/env python3
#    This is 'comp', a tool to write HAL boilerplate
#    Copyright 2006 Jeff Epler <jepler@unpythonic.net>
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program; if not, write to the Free Software
#    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

%%
parser Hal:
    ignore: "//.*"
    ignore: "/[*](.|\n)*?[*]/"
    ignore: "[ \t\r\n]+"

    token END: ";;"
    token PARAMDIRECTION: "rw|r"
    token PINDIRECTION: "in|out|io"
    token TYPE: "float|bit|signed|unsigned|u32|s32|u64|s64"
    token NAME: "[a-zA-Z_][a-zA-Z0-9_]*"
    token STARREDNAME: "[*]*[a-zA-Z_][a-zA-Z0-9_]*"
    token HALNAME: "[#a-zA-Z_][-#a-zA-Z0-9_.]*"
    token FPNUMBER: "-?([0-9]*\.[0-9]+|[0-9]+\.?)([Ee][+-]?[0-9]+)?f?"
    token NUMBER: "0x[0-9a-fA-F]+|[+-]?[0-9]+"
    token STRING: "\"(\\.|[^\\\"])*\""
    token HEADER: "<.*?>"
    token POP: "[-()+*/]|&&|\\|\\||personality|==|&|!=|<|<=|>|>="
    token TSTRING: "\"\"\"(\\.|\\\n|[^\\\"]|\"(?!\"\")|\n)*\"\"\""

    rule File: ComponentDeclaration Declaration* "$" {{ return True }}
    rule ComponentDeclaration:
        "component" NAME OptString";" {{ comp(NAME, OptString); }}
    rule Declaration:
        "pin" PINDIRECTION TYPE HALNAME OptArray OptSAssign OptPersonality OptString ";"  {{ pin(HALNAME, TYPE, OptArray, PINDIRECTION, OptString, OptSAssign, OptPersonality) }}
      | "param" PARAMDIRECTION TYPE HALNAME OptArray OptSAssign OptPersonality OptString ";" {{ param(HALNAME, TYPE, OptArray, PARAMDIRECTION, OptString, OptSAssign, OptPersonality) }}
      | "function" NAME OptFP OptString ";"       {{ function(NAME, OptFP, OptString) }}
      | "variable" NAME STARREDNAME OptSimpleArray OptAssign ";" {{ variable(NAME, STARREDNAME, OptSimpleArray, OptAssign) }}
      | "option" NAME OptValue ";"   {{ option(NAME, OptValue) }}
      | "see_also" String ";"   {{ see_also(String) }}
      | "notes" String ";"   {{ notes(String) }}
      | "description" String ";"   {{ description(String) }}
      | "special_format_doc" String ";"   {{ special_format_doc(String) }}
      | "license" String ";"   {{ license(String) }}
      | "author" String ";"   {{ author(String) }}
      | "include" Header ";"   {{ include(Header) }}
      | "modparam" NAME {{ NAME1=NAME; }} NAME OptSAssign OptString ";" {{ modparam(NAME1, NAME, OptSAssign, OptString) }}

    rule Header: STRING {{ return STRING }} | HEADER {{ return HEADER }}

    rule String: TSTRING {{ return eval(TSTRING) }}
            | STRING {{ return eval(STRING) }}

    rule OptPersonality: "if" Personality {{ return Personality }}
            | {{ return None }}
    rule Personality: {{ pp = [] }} (PersonalityPart {{ pp.append(PersonalityPart) }} )* {{ return " ".join(pp) }}
    rule PersonalityPart: NUMBER {{ return NUMBER }}
            | POP {{ return POP }}
    rule OptSimpleArray: "\[" NUMBER "\]" {{ return int(NUMBER) }}
            | {{ return 0 }}
    rule OptArray: "\[" NUMBER OptArrayPersonality "\]" {{ return OptArrayPersonality and (int(NUMBER), OptArrayPersonality) or int(NUMBER) }}
            | {{ return 0 }}
    rule OptArrayPersonality: ":" Personality {{ return Personality }}
            | {{ return None }}
    rule OptString: TSTRING {{ return eval(TSTRING) }}
            | STRING {{ return eval(STRING) }}
            | {{ return '' }}
    rule OptAssign: "=" Value {{ return Value; }}
                | {{ return None }}
    rule OptSAssign: "=" SValue {{ return SValue; }}
                | {{ return None }}
    rule OptFP: "fp" {{ return 1 }} | "nofp" {{ return 0 }} | {{ return 1 }}
    rule Value: "yes" {{ return 1 }} | "no" {{ return 0 }}
                | "true" {{ return 1 }} | "false" {{ return 0 }}
                | "TRUE" {{ return 1 }} | "FALSE" {{ return 0 }}
                | NAME {{ return NAME }}
                | FPNUMBER {{ return float(FPNUMBER.rstrip("f")) }}
                | NUMBER {{ return int(NUMBER,0) }}
    rule SValue: "yes" {{ return "yes" }} | "no" {{ return "no" }}
                | "true" {{ return "true" }} | "false" {{ return "false" }}
                | "TRUE" {{ return "TRUE" }} | "FALSE" {{ return "FALSE" }}
                | NAME {{ return NAME }}
                | FPNUMBER {{ return FPNUMBER }}
                | NUMBER {{ return NUMBER }}
    rule OptValue: Value {{ return Value }}
                | TSTRING {{ return eval(TSTRING) }}
                | STRING {{ return eval(STRING) }}
                | {{ return 1 }}
    rule OptSValue: SValue {{ return SValue }}
                | {{ return 1 }}
%%

import os, sys, tempfile, shutil, getopt, time, stat
BASE = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]), ".."))
sys.path.insert(0, os.path.join(BASE, "lib", "python"))

mp_decl_map = {'int': 'RTAPI_MP_INT', 'dummy': None}

# These are symbols that comp puts in the global namespace of the C file it
# creates.  The user is thus not allowed to add any symbols with these
# names.  That includes not only global variables and functions, but also
# HAL pins & parameters, because comp adds #defines with the names of HAL
# pins & params.
reserved_names = [ 'comp_id', 'fperiod', 'rtapi_app_main', 'rtapi_app_exit', 'extra_setup', 'extra_cleanup' ]

def _parse(rule, text, filename=None):
    global P, S
    S = HalScanner(text, filename=filename)
    P = Hal(S)
    return runtime.wrap_error_reporter(P, rule)

def parse(filename):
    initialize()
    f = open(filename).read()
    if '\r' in f:
        raise SystemExit("Error: Mac or DOS style line endings in file %s" % filename)
    a, b = f.split("\n;;\n", 1)
    p = _parse('File', a + "\n\n", filename)
    if not p: raise SystemExit(1)
    if require_license:
        if not finddoc('license'):
            raise SystemExit("Error: %s:0: License not specified" % filename)
    return a, b

dirmap = {'r': 'HAL_RO', 'rw': 'HAL_RW', 'in': 'HAL_IN', 'out': 'HAL_OUT', 'io': 'HAL_IO' }
typemap = {'signed': 's32', 'unsigned': 'u32'}
deprmap = {'s32': 'signed', 'u32': 'unsigned'}
deprecated = ['s32', 'u32']

def initialize():
    global functions, params, pins, options, comp_name, names, docs, variables
    global modparams, includes

    functions = []; params = []; pins = []; options = {}; variables = []
    modparams = []; docs = []; includes = [];
    comp_name = None

    names = {}

def Warn(msg, *args):
    if args:
        msg = msg % args
    sys.stderr.write("%s:%d: Warning: %s\n" % (S.filename, S.line, msg))

def Error(msg, *args):
    if args:
        msg = msg % args
    raise runtime.SyntaxError(S.get_pos(), msg, None)

def comp(name, doc):
    docs.append(('component', name, doc))
    global comp_name
    if comp_name:
        Error("Duplicate specification of component name")
    comp_name = name;

def description(doc):
    docs.append(('descr', doc));

def special_format_doc(doc):
    docs.append(('sf_doc', doc));

def license(doc):
    docs.append(('license', doc));

def author(doc):
    docs.append(('author', doc));

def see_also(doc):
    docs.append(('see_also', doc));

def notes(doc):
    docs.append(('notes', doc));

def type2type(type):
    # When we start warning about s32/u32 this is where the warning goes
    return typemap.get(type, type)

def checkarray(name, array):
    hashes = len(re.findall("#+", name))
    if array:
        if hashes == 0: Error("Array name contains no #: %r" % name)
        if hashes > 1: Error("Array name contains more than one block of #: %r" % name)
    else:
        if hashes > 0: Error("Non-array name contains #: %r" % name)

def check_name_ok(name):
    if name in reserved_names:
        Error("Variable name %s is reserved" % name)
    if name in names:
        Error("Duplicate item name %s" % name)

def pin(name, type, array, dir, doc, value, personality):
    checkarray(name, array)
    type = type2type(type)
    check_name_ok(name)
    docs.append(('pin', name, type, array, dir, doc, value, personality))
    names[name] = None
    pins.append((name, type, array, dir, value, personality))

def param(name, type, array, dir, doc, value, personality):
    checkarray(name, array)
    type = type2type(type)
    check_name_ok(name)
    docs.append(('param', name, type, array, dir, doc, value, personality))
    names[name] = None
    params.append((name, type, array, dir, value, personality))

def function(name, fp, doc):
    check_name_ok(name)
    docs.append(('funct', name, fp, doc))
    names[name] = None
    functions.append((name, fp))

def option(name, value):
    if name in options:
        Error("Duplicate option name %s" % name)
    options[name] = value

def variable(type, name, array, default):
    check_name_ok(name)
    names[name] = None
    variables.append((type, name, array, default))

def modparam(type, name, default, doc):
    check_name_ok(name)
    names[name] = None
    modparams.append((type, name, default, doc))

def include(value):
    includes.append((value))

def removeprefix(s,p):
    if s.startswith(p): return s[len(p):]
    return s

def to_hal(name):
    name = re.sub("#+", lambda m: "%%0%dd" % len(m.group(0)), name)
    return name.replace("_", "-").rstrip("-").rstrip(".")
def to_c(name):
    name = re.sub("[-._]*#+", "", name)
    name = name.replace("#", "").replace(".", "_").replace("-", "_")
    return re.sub("_+", "_", name)

def prologue(f):
    f.write("/* Autogenerated by %s -- do not edit */\n" % (
        sys.argv[0]))
    f.write("""\
#include "rtapi.h"
#ifdef RTAPI
#include "rtapi_app.h"
#endif
#include "rtapi_string.h"
#include "rtapi_errno.h"
#include "hal.h"

static int comp_id;
""")

    for name in includes:
        f.write("#include %s\n" % name)

    names = {}

    def q(s):
        s = s.replace("\\", "\\\\")
        s = s.replace("\"", "\\\"")
        s = s.replace("\r", "\\r")
        s = s.replace("\n", "\\n")
        s = s.replace("\t", "\\t")
        s = s.replace("\v", "\\v")
        return '"%s"' % s

    f.write("#ifdef MODULE_INFO\n")
    for v in docs:
        if not v: continue
        v = ":".join(map(str, v))
        f.write("MODULE_INFO(machinekit, %s);\n" % q(v))
        license = finddoc('license')
    if license and license[1]:
        f.write("MODULE_LICENSE(\"%s\");\n" % license[1].split("\n")[0])
    f.write("#endif // MODULE_INFO\n")
    f.write("\n")


    has_data = options.get("data")

    has_array = False
    has_personality = False
    for name, type, array, dir, value, personality in pins:
        if array: has_array = True
        if isinstance(array, tuple): has_personality = True
        if personality: has_personality = True
    for name, type, array, dir, value, personality in params:
        if array: has_array = True
        if isinstance(array, tuple): has_personality = True
        if personality: has_personality = True
    for type, name, default, doc in modparams:
        decl = mp_decl_map[type]
        if decl:
            f.write("%s %s" % (type, name))
            if default: f.write("= %s;\n" % default)
            else: f.write(";\n")
            f.write("%s(%s, %s);\n" % (decl, name, q(doc)))

    f.write("\n")
    f.write("struct __comp_state {\n")
    f.write("    struct __comp_state *_next;\n")
    if has_personality:
        f.write("    int _personality;\n")

    for name, type, array, dir, value, personality in pins:
        if array:
            if isinstance(array, tuple): array = array[0]
            f.write("    hal_%s_t *%s[%s];\n" % (type, to_c(name), array))
        else:
            f.write("    hal_%s_t *%s;\n" % (type, to_c(name)))
        names[name] = 1

    for name, type, array, dir, value, personality in params:
        if array:
            if isinstance(array, tuple): array = array[0]
            f.write("    hal_%s_t %s[%s];\n" % (type, to_c(name), array))
        else:
            f.write("    hal_%s_t %s;\n" % (type, to_c(name)))
        names[name] = 1

    for type, name, array, value in variables:
        if array:
            f.write("    %s %s[%d];\n\n" % (type, name, array))
        else:
            f.write("    %s %s;\n\n" % (type, name))
    if has_data:
        f.write("    void *_data;\n")

    f.write("};\n")

    if options.get("userspace"):
        f.write("#include <stdlib.h>\n")

    f.write("struct __comp_state *__comp_inst=0;\n")
    f.write("struct __comp_state *__comp_first_inst=0, *__comp_last_inst=0;\n")

    f.write("\n")
    for name, fp in functions:
        if name in names:
            Error("Duplicate item name: %s" % name)
        f.write("static void %s(struct __comp_state *__comp_inst, long period);\n" % to_c(name))
        names[name] = 1

    f.write("static int __comp_get_data_size(void);\n")
    if options.get("extra_setup"):
        f.write("static int extra_setup(struct __comp_state *__comp_inst, char *prefix, long extra_arg);\n")
    if options.get("extra_cleanup"):
        f.write("static void extra_cleanup(void);\n")

    if not options.get("no_convenience_defines"):
        f.write("#undef TRUE\n")
        f.write("#define TRUE (1)\n")
        f.write("#undef FALSE\n")
        f.write("#define FALSE (0)\n")
        f.write("#undef true\n")
        f.write("#define true (1)\n")
        f.write("#undef false\n")
        f.write("#define false (0)\n")

    f.write("\n")
    if has_personality:
        f.write("static int export(char *prefix, long extra_arg, long personality) {\n")
    else:
        f.write("static int export(char *prefix, long extra_arg) {\n")
    if len(functions) > 0:
        f.write("    char buf[HAL_NAME_LEN + 1];\n")
    f.write("    int r = 0;\n")
    if has_array:
        f.write("    int j = 0;\n")
    f.write("    int sz = sizeof(struct __comp_state) + __comp_get_data_size();\n")
    f.write("    struct __comp_state *inst = hal_malloc(sz);\n")
    f.write("    memset(inst, 0, sz);\n")
    if has_data:
        f.write("    inst->_data = (char*)inst + sizeof(struct __comp_state);\n")
    if has_personality:
        f.write("    inst->_personality = personality;\n")
    if options.get("extra_setup"):
        f.write("    r = extra_setup(inst, prefix, extra_arg);\n")
        f.write("    if(r != 0) return r;\n")
        # the extra_setup() function may have changed the personality
        if has_personality:
            f.write("    personality = inst->_personality;\n")
    for name, type, array, dir, value, personality in pins:
        if personality:
            f.write("if(%s) {\n" % personality)
        if array:
            if isinstance(array, tuple): array = array[1]
            f.write("    for(j=0; j < (%s); j++) {\n" % array)
            f.write("        r = hal_pin_%s_newf(%s, &(inst->%s[j]), comp_id,\n" % (
                type, dirmap[dir], to_c(name)))
            f.write("            \"%%s%s\", prefix, j);\n" % to_hal("." + name))
            f.write("        if(r != 0) return r;\n")
            if value is not None:
                f.write("    *(inst->%s[j]) = %s;\n" % (to_c(name), value))
            f.write("    }\n")
        else:
            f.write("    r = hal_pin_%s_newf(%s, &(inst->%s), comp_id,\n" % (
                type, dirmap[dir], to_c(name)))
            f.write("        \"%%s%s\", prefix);\n" % to_hal("." + name))
            f.write("    if(r != 0) return r;\n")
            if value is not None:
                f.write("    *(inst->%s) = %s;\n" % (to_c(name), value))
        if personality:
            f.write("}\n")

    for name, type, array, dir, value, personality in params:
        if personality:
            f.write("if(%s) {\n" % personality)
        if array:
            if isinstance(array, tuple): array = array[1]
            f.write("    for(j=0; j < %s; j++) {\n" % array)
            f.write("        r = hal_param_%s_newf(%s, &(inst->%s[j]), comp_id,\n" % (
                type, dirmap[dir], to_c(name)))
            f.write("            \"%%s%s\", prefix, j);\n" % to_hal("." + name))
            f.write("        if(r != 0) return r;\n")
            if value is not None:
                f.write("    inst->%s[j] = %s;\n" % (to_c(name), value))
            f.write("    }\n")
        else:
            f.write("    r = hal_param_%s_newf(%s, &(inst->%s), comp_id,\n" % (
                type, dirmap[dir], to_c(name)))
            f.write("        \"%%s%s\", prefix);\n" % to_hal("." + name))
            if value is not None:
                f.write("    inst->%s = %s;\n" % (to_c(name), value))
            f.write("    if(r != 0) return r;\n")
        if personality:
            f.write("}\n")

    for type, name, array, value in variables:
        if value is None: continue
        if array:
            f.write("    for(j=0; j < %s; j++) {\n" % array)
            f.write("        inst->%s[j] = %s;\n" % (name, value))
            f.write("    }\n")
        else:
            f.write("    inst->%s = %s;\n" % (name, value))

    for name, fp in functions:
        f.write("    rtapi_snprintf(buf, sizeof(buf), \"%%s%s\", prefix);\n"\
                % to_hal("." + name))
        f.write("    r = hal_export_funct(buf, (void(*)(void *inst, long))%s, inst, %s, 0, comp_id);\n" % (
            to_c(name), int(fp)))
        f.write("    if(r != 0) return r;\n")
    f.write("    if(__comp_last_inst) __comp_last_inst->_next = inst;\n")
    f.write("    __comp_last_inst = inst;\n")
    f.write("    if(!__comp_first_inst) __comp_first_inst = inst;\n")
    f.write("    if(!__comp_inst) __comp_inst = inst;\n")
    f.write("    return 0;\n")
    f.write("}\n")

    if options.get("count_function"):
        f.write("static int get_count(void);\n")

    if options.get("rtapi_app", 1):
        if options.get("constructable") and not options.get("singleton"):
            f.write("static int export_1(char *prefix, char *argstr) {\n")
            f.write("    int arg = simple_strtol(argstr, NULL, 0);\n")
            f.write("    return export(prefix, arg);\n")
            f.write("}\n")
        if not options.get("singleton") and not options.get("count_function") :
            f.write("static int default_count=%s, count=0;\n" \
                % options.get("default_count", 1))
            f.write("char *names[16] = {0,};\n")
            if not options.get("userspace"):
                f.write("RTAPI_MP_INT(count, \"number of %s\");\n" % comp_name)
                f.write("RTAPI_MP_ARRAY_STRING(names, 16, \"names of %s\");\n" % comp_name)

        if has_personality:
            init1 = str(int(options.get('default_personality', 0)))
            init = ",".join([init1] * 16)
            f.write("static int personality[16] = {%s};\n" % init)
            f.write("RTAPI_MP_ARRAY_INT(personality, 16, \"personality of each %s\");\n" % comp_name)
        f.write("int rtapi_app_main(void) {\n")
        f.write("    int r = 0;\n")
        if not options.get("singleton"):
            f.write("    int i;\n")
        if options.get("count_function"):
            f.write("    int count = get_count();\n")

        f.write("    comp_id = hal_init(\"%s\");\n" % comp_name)
        f.write("    if(comp_id < 0) return comp_id;\n")

        if options.get("singleton"):
            if has_personality:
                f.write("    r = export(\"%s\", 0, personality[0]);\n" % \
                        to_hal(removeprefix(comp_name, "hal_")))
            else:
                f.write("    r = export(\"%s\", 0);\n" % \
                        to_hal(removeprefix(comp_name, "hal_")))
        elif options.get("count_function"):
            f.write("    for(i=0; i<count; i++) {\n")
            f.write("        char buf[HAL_NAME_LEN + 1];\n")
            f.write("        rtapi_snprintf(buf, sizeof(buf), " \
                    "\"%s.%%d\", i);\n" % \
                    to_hal(removeprefix(comp_name, "hal_")))
            if has_personality:
                f.write("        r = export(buf, i, personality[i%16]);\n")
            else:
                f.write("        r = export(buf, i);\n")
            f.write("    }\n")
        else:
            f.write("    if(count && names[0]) {\n")
            f.write("        rtapi_print_msg(RTAPI_MSG_ERR," \
                    "\"count= and names= are mutually exclusive\\n\");\n")
            f.write("        return -EINVAL;\n")
            f.write("    }\n")
            f.write("    if(!count && !names[0]) count = default_count;\n")
            f.write("    if(count) {\n")
            f.write("        for(i=0; i<count; i++) {\n")
            f.write("            char buf[HAL_NAME_LEN + 1];\n")
            f.write("            rtapi_snprintf(buf, sizeof(buf), " \
                    "\"%s.%%d\", i);\n" % \
                    to_hal(removeprefix(comp_name, "hal_")))
            if has_personality:
                f.write("        r = export(buf, i, personality[i%16]);\n")
            else:
                f.write("        r = export(buf, i);\n")
            f.write("            if(r != 0) break;\n")
            f.write("       }\n")
            f.write("    } else {\n")
            f.write("        for(i=0; names[i]; i++) {\n")
            if has_personality:
                f.write("        r = export(names[i], i, personality[i%16]);\n")
            else:
                f.write("        r = export(names[i], i);\n")
            f.write("            if(r != 0) break;\n")
            f.write("       }\n")
            f.write("    }\n")

        if options.get("constructable") and not options.get("singleton"):
            f.write("    hal_set_constructor(comp_id, export_1);\n")
        f.write("    if(r) {\n")
        if options.get("extra_cleanup"):
            f.write("    extra_cleanup();\n")
        f.write("        hal_exit(comp_id);\n")
        f.write("    } else {\n")
        f.write("        hal_ready(comp_id);\n")
        f.write("    }\n")
        f.write("    return r;\n")
        f.write("}\n")

        f.write("\n")
        f.write("void rtapi_app_exit(void) {\n")
        if options.get("extra_cleanup"):
            f.write("    extra_cleanup();\n")
        f.write("    hal_exit(comp_id);\n")
        f.write("}\n")

    if options.get("userspace"):
        f.write("static void user_mainloop(void);\n")
        if options.get("userinit"):
            f.write("static void userinit(int argc, char **argv);\n")
        f.write("int argc=0; char **argv=0;\n")
        f.write("int main(int argc_, char **argv_) {\n")
        f.write("    argc = argc_; argv = argv_;\n")
        f.write("\n")
        if options.get("userinit", 0):
            f.write("    userinit(argc, argv);\n")
        f.write("\n")
        f.write("    if(rtapi_app_main() < 0) return 1;\n")
        f.write("    user_mainloop();\n")
        f.write("    rtapi_app_exit();\n")
        f.write("    return 0;\n")
        f.write("}\n")

    f.write("\n")
    if not options.get("no_convenience_defines"):
        f.write("#undef FUNCTION\n")
        f.write("#define FUNCTION(name) static void name(struct __comp_state *__comp_inst, long period)\n")
        f.write("#undef EXTRA_SETUP\n")
        f.write("#define EXTRA_SETUP() static int extra_setup(struct __comp_state *__comp_inst, char *prefix, long extra_arg)\n")
        f.write("#undef EXTRA_CLEANUP\n")
        f.write("#define EXTRA_CLEANUP() static void extra_cleanup(void)\n")
        f.write("#undef fperiod\n")
        f.write("#define fperiod (period * 1e-9)\n")
        for name, type, array, dir, value, personality in pins:
            f.write("#undef %s\n" % to_c(name))
            if array:
                if dir == 'in':
                    f.write("#define %s(i) (0+*(__comp_inst->%s[i]))\n" % (to_c(name), to_c(name)))
                else:
                    f.write("#define %s(i) (*(__comp_inst->%s[i]))\n" % (to_c(name), to_c(name)))
            else:
                if dir == 'in':
                    f.write("#define %s (0+*__comp_inst->%s)\n" % (to_c(name), to_c(name)))
                else:
                    f.write("#define %s (*__comp_inst->%s)\n" % (to_c(name), to_c(name)))
        for name, type, array, dir, value, personality in params:
            f.write("#undef %s\n" % to_c(name))
            if array:
                f.write("#define %s(i) (__comp_inst->%s[i])\n" % (to_c(name), to_c(name)))
            else:
                f.write("#define %s (__comp_inst->%s)\n" % (to_c(name), to_c(name)))

        for type, name, array, value in variables:
            name = name.replace("*", "")
            f.write("#undef %s\n" % name)
            f.write("#define %s (__comp_inst->%s)\n" % (name, name))

        if has_data:
            f.write("#undef data\n")
            f.write("#define data (*(%s*)(__comp_inst->_data))\n" % options['data'])
        if has_personality:
            f.write("#undef personality\n")
            f.write("#define personality (__comp_inst->_personality)\n")

        if options.get("userspace"):
            f.write("#undef FOR_ALL_INSTS\n")
            f.write("#define FOR_ALL_INSTS() for(__comp_inst = __comp_first_inst; __comp_inst; __comp_inst = __comp_inst->_next)\n")
    f.write("\n")
    f.write("\n")

def epilogue(f):
    data = options.get('data')
    f.write("\n")
    if data:
        f.write("static int __comp_get_data_size(void) { return sizeof(%s); }\n" % data)
    else:
        f.write("static int __comp_get_data_size(void) { return 0; }\n")

INSTALL, COMPILE, PREPROCESS, DOCUMENT, INSTALLDOC, VIEWDOC, MODINC = range(7)
modename = ("install", "compile", "preprocess", "document", "installdoc", "viewdoc", "print-modinc")

modinc = None
def find_modinc():
    global modinc
    if modinc: return modinc
    d = os.path.abspath(os.path.dirname(os.path.dirname(sys.argv[0])))
    for e in ['src', 'etc/machinekit', '/etc/machinekit', 'share/machinekit']:
        e = os.path.join(d, e, 'Makefile.modinc')
        if os.path.exists(e):
            modinc = e
            return e
    raise SystemExit("Error: Unable to locate Makefile.modinc")

def build_usr(tempdir, filename, mode, origfilename, cflags, compname=None):
    if compname:
        binname = compname
    else:
        binname = os.path.basename(os.path.splitext(filename)[0])

    makefile = os.path.join(tempdir, "Makefile")
    f = open(makefile, "w")
    f.write("%s: %s\n" % (binname, filename))
    f.write("\t$(CC) $(EXTRA_CFLAGS) -URTAPI -U__MODULE__ -DULAPI -Os %s -o $@ $< -Wl,-rpath,$(LIBDIR) -L$(LIBDIR) -lhal -lhalulapi %s\n" % (
        options.get("extra_compile_args", ""),
        options.get("extra_link_args", "")))
    f.write("include %s\n" % find_modinc())
    f.write("EXTRA_CFLAGS += -I%s\n" % os.path.abspath(os.path.dirname(origfilename)))
    f.write("EXTRA_CFLAGS += -I%s\n" % os.path.abspath('.'))
    if cflags:
        f.write("EXTRA_CFLAGS += %s\n" % cflags)
    f.close()
    result = os.system("cd %s && make -S %s" % (tempdir, binname))
    if result != 0:
        raise SystemExit(os.WEXITSTATUS(result) or 1)
    output = os.path.join(tempdir, binname)
    if mode == INSTALL:
        shutil.copy(output, os.path.join(BASE, "bin", binname))
    elif mode == COMPILE:
        shutil.copy(output, os.path.join(os.path.dirname(origfilename),binname))

def build_rt(tempdir, filename, mode, c_sources, cflags, compname=None):
    if compname:
        objname = compname + ".o"
    else:
        objname = os.path.basename(os.path.splitext(filename)[0] + ".o")
        c_sources = [c_sources,]
    origdir = os.path.abspath(os.path.dirname(c_sources[0]))
    c_objs = [s[:-2]+'.o' for s in c_sources]
    makefile = os.path.join(tempdir, "Makefile")
    f = open(makefile, "w")
    f.write("obj-m += %s.o\n" % compname)
    f.write("%s-objs := \\\n" % compname)
    f.write("    %s\n" % " ".join(c_objs))
    f.write("include %s\n" % find_modinc())
    f.write("EXTRA_CFLAGS += -I%s\n" % origdir)
    f.write("EXTRA_CFLAGS += -I%s\n" % os.path.abspath('.'))
    f.write("EXTRA_CFLAGS += -DRTAPI -L$(LIBDIR) -lhal\n")
    if cflags:
        f.write("EXTRA_CFLAGS += %s\n" % cflags)
    f.close()
    if mode == INSTALL:
        target = "modules install"
    else:
        target = "modules"
    result = os.system("cd %s && make -S %s" % (tempdir, target))
    if result != 0:
        raise SystemExit(os.WEXITSTATUS(result) or 1)
    if mode == COMPILE:
        for extension in ".ko", ".so", ".o":
            if compname:
                kobjname = os.path.join(
                    os.path.dirname(filename), compname + extension)
            else:
                kobjname = os.path.splitext(filename)[0] + extension
            if os.path.exists(kobjname):
                shutil.copy(kobjname, os.path.basename(kobjname))
                break
        else:
            raise SystemExit("Error: Unable to copy module %s from temporary directory" % kobjname)

############################################################

def finddoc(section=None, name=None):
    for item in docs:
        if ((section == None or section == item[0]) and
                (name == None or name == item[1])): return item
    return None

def finddocs(section=None, name=None):
    for item in docs:
        if ((section == None or section == item[0]) and
                (name == None or name == item[1])):
                    yield item

def to_hal_man_unnumbered(s):
    s = "%s.%s" % (comp_name, s)
    s = s.replace("_", "-")
    s = s.rstrip("-")
    s = s.rstrip(".")
    s = re.sub("#+", lambda m: "\\fI" + "M" * len(m.group(0)) + "\\fB", s)
    # s = s.replace("-", "\\-")
    return s


def to_hal_man(s):
    if options.get("singleton"):
        s = "%s.%s" % (comp_name, s)
    else:
        s = "%s.\\fIN\\fB.%s" % (comp_name, s)
    s = s.replace("_", "-")
    s = s.rstrip("-")
    s = s.rstrip(".")
    s = re.sub("#+", lambda m: "\\fI" + "M" * len(m.group(0)) + "\\fB", s)
    # s = s.replace("-", "\\-")
    return s

##############################################################################
# asciidoc
############

def adocument(filename, outfilename, frontmatter):
    if outfilename is None:
        outfilename = os.path.splitext(filename)[0] + ".asciidoc"

    a, b = parse(filename)
    f = open(outfilename, "w")

    has_personality = False
    for name, type, array, dir, value, personality in pins:
        if personality: has_personality = True
        if isinstance(array, tuple): has_personality = True
    for name, type, array, dir, value, personality in params:
        if personality: has_personality = True
        if isinstance(array, tuple): has_personality = True

    if frontmatter:
        f.write("---\n")
        for fm in frontmatter:
            f.write("%s\n" % fm)
        f.write("edit-path: src/%s\n" % (filename))
        f.write("generator: comp\n")
        f.write("description: This page was generated from src/%s. Do not edit directly, edit the source.\n" % filename)
        f.write("---\n")
        f.write(":skip-front-matter:\n\n")

    f.write("= Machinekit Documentation\n")

    f.write("\n")
    f.write("== HAL Component -- %s\n" % (comp_name.upper()))
    f.write("\n")

    f.write("=== NAME\n")
    f.write("\n")
    doc = finddoc('component')
    if doc and doc[2]:
        if '\n' in doc[2]:
            firstline, rest = doc[2].split('\n', 1)
        else:
            firstline = doc[2]
            rest = ''
        f.write("==== %s -- %s\n" % (doc[1], firstline))
    else:
        rest = ''
        f.write("==== %s\n" % doc[1])
    f.write("\n")

    f.write("=== SYNOPSIS\n")
    f.write("\n")
    if rest:
        f.write("*%s*\n" % rest)
    else:
        rest = ''
        f.write("*%s*\n" % doc[1])
    f.write("\n"    )

    f.write("=== USAGE SYNOPSIS\n")
    f.write("\n")
    if rest:
        f.write("*%s*\n" % rest)
        f.write("\n"                    )
    else:
        if options.get("userspace"):
            f.write("%s [-W]\n" % comp_name)
            f.write("\n")
        else:
            if rest:
                f.write("%s\n" % rest)
                f.write("\n")
            else:
                if options.get("singleton") or options.get("count_function"):
                    if has_personality:
                        f.write("*loadrt %s personality=_P_*\n" % comp_name)
                    else:
                        f.write("*loadrt %s*\n" % comp_name)
                else:
                    if has_personality:
                        f.write("*loadrt %s [count=_N_|names=_name1_[,_name2..._]] [personality=_P,P,..._]*\n" % comp_name)
                    else:
                        f.write("*loadrt %s [count=_N_|names=_name1_[,_name2..._]]*\n" % comp_name)

                for type, name, default, doc in modparams:
                    f.write("[%s=_N_]" % name)
                f.write("\n")

                hasparamdoc = False
                for type, name, default, doc in modparams:
                    if doc: hasparamdoc = True

                if hasparamdoc:
                    for type, name, default, doc in modparams:
                        f.write("\n")
                        f.write("*%s*" % name)
                        if default:
                            f.write("*[default: %s]*\n" % default)
                        else:
                            f.write("\n")
                        f.write("%s\n" % doc)
                    f.write("\n")

            if options.get("constructable") and not options.get("singleton"):
                f.write("\n*newinst %s _name_*\n" % comp_name)
        f.write("\n")

    doc = finddoc('descr')
    if doc and doc[1]:
        f.write("=== DESCRIPTION\n")
        f.write("\n")
        f.write("%s\n" % doc[1])
        f.write("\n")

    doc = finddoc('sf_doc')
    if doc and doc[1]:
        f.write("=== EXTRA INFO\n")
        f.write("\n")
        f.write("%s\n" % doc[1])
        f.write("\n")

    if functions:
        f.write("=== FUNCTIONS\n")
        f.write("\n")
        for _, name, fp, doc in finddocs('funct'):
            if name != None and name != "_":
                f.write("*%s.N.%s*\n" % (comp_name, to_hal(name)))
            else :
                f.write("*%s.N*\n" % comp_name )
            if fp:
                f.write("(requires a floating-point thread)\n")
            else:
                f.write("\n")
            f.write("%s\n" % doc)
            f.write("\n")
        f.write("\n")

    f.write("=== PINS\n")
    f.write("\n"    )
    for _, name, type, array, dir, doc, value, personality in finddocs('pin'):
        f.write("*%s*" % to_hal(name))
        f.write("%s %s" % (type, dir))
        if array:
            sz = name.count("#")
            if isinstance(array, tuple):
                f.write(" (%s=%0*d..%s)" % ("M" * sz, sz, 0, array[1]))
            else:
                f.write(" (%s=%0*d..%0*d)" % ("M" * sz, sz, 0, sz, array-1))
        if personality:
            f.write(" [if %s]" % personality)
        if value:
            f.write("*(default: _%s_)*\n" % value)
        if doc:
            f.write(" - %s\n\n" % doc)
        else:
            f.write("\n\n")

    f.write("\n\n")

    if params:
        f.write("=== PARAMETERS\n")
        f.write("\n")
        for _, name, type, array, dir, doc, value, personality in finddocs('param'):
            f.write("*%s*" % to_hal(name))
            f.write("%s %s" % (type, dir))
            if array:
                sz = name.count("#")
                if isinstance(array, tuple):
                    f.write(" (%s=%0*d..%s)" % ("M" * sz, sz, 0, array[1]))
                else:
                    f.write(" (%s=%0*d..%0*d)" % ("M" * sz, sz, 0, sz, array-1))
            if personality:
                f.write(" [if %s]" % personality)
            if value:
                f.write("*(default: _%s_)*\n" % value)
            if doc:
                f.write(" - %s\n\n" % doc)
            else:
                f.write("\n\n")

        f.write("\n\n")

    doc = finddoc('see_also')
    if doc and doc[1]:
        f.write("\n")
        f.write("=== SEE ALSO\n")
        f.write("\n")
        f.write("%s\n" % doc[1])
        f.write("\n"    )

    doc = finddoc('notes')
    if doc and doc[1]:
        f.write("\n"    )
        f.write("=== NOTES\n")
        f.write("\n")
        f.write("%s\n" % doc[1])
        f.write("\n"    )

    doc = finddoc('author')
    if doc and doc[1]:
        f.write("\n"    )
        f.write("=== AUTHOR\n")
        f.write("\n")
        f.write("%s\n" % doc[1])
        f.write("\n"    )

    doc = finddoc('license')
    if doc and doc[1]:
        f.write("\n"    )
        f.write("=== LICENCE\n")
        f.write("\n")
        f.write("%s\n" % doc[1])
        f.write("\n"    )



###########################################################


def process(filename, mode, outfilename, cflags):
    tempdir = tempfile.mkdtemp()
    try:
        if outfilename is None:
            if mode == PREPROCESS:
                outfilename = os.path.splitext(filename)[0] + ".c"
            else:
                outfilename = os.path.join(tempdir,
                    os.path.splitext(os.path.basename(filename))[0] + ".c")

        a, b = parse(filename)
        base_name = os.path.splitext(os.path.basename(outfilename))[0]
        if comp_name != base_name:
            raise SystemExit("Error: Component name (%s) does not match filename (%s)" % (comp_name, base_name))
        f = open(outfilename, "w")

        if options.get("userinit") and not options.get("userspace"):
            sys.stderr.write("Warning: comp '%s' sets 'userinit' without 'userspace', ignoring" % filename)

        if options.get("userspace"):
            if functions:
                raise SystemExit("Error: Userspace components may not have functions")
        if not pins:
            raise SystemExit("Error: Component must have at least one pin")
        prologue(f)
        lineno = a.count("\n") + 3

        if options.get("userspace"):
            if functions:
                raise SystemExit("Error: May not specify functions with a userspace component.")
            f.write("#line %d \"%s\"\n" % (lineno, filename))
            f.write(b)
        else:
            if not functions or "FUNCTION" in b:
                f.write("#line %d \"%s\"\n" % (lineno, filename))
                f.write(b)
            elif len(functions) == 1:
                f.write("FUNCTION(%s) {\n" % functions[0][0])
                f.write("#line %d \"%s\"\n" % (lineno, filename))
                f.write(b)
                f.write("}\n")
            else:
                raise SystemExit("Error: Must use FUNCTION() when more than one function is defined")
        epilogue(f)
        f.close()

        if mode != PREPROCESS:
            if options.get("userspace"):
                build_usr(tempdir, outfilename, mode, filename, cflags)
            else:
                build_rt(tempdir, outfilename, mode, filename, cflags)

    finally:
        shutil.rmtree(tempdir)

def usage(exitval=0):
    print ("""{0}: Build, compile, and install Machinekit HAL components

Usage:
           {0} [ --compile (-c) | --preprocess (-p) | --document (-d) | --view-doc (-v) ] compfile...
    [sudo] {0} [ --install (-i) | --install-doc (-j) ] compfile..
           {0} --compile [--userspace] [--cflags=...] [--compname=...] cfile...
    [sudo] {0} --install [--userspace] [--cflags=...] [--compname=...] cfile...
    [sudo] {0} --install --userspace pyfile...
           {0} --print-modinc
""".format(os.path.basename(sys.argv[0])))
    raise SystemExit(exitval)

def main():
    global require_license
    require_license = True
    mode = PREPROCESS
    outfile = None
    frontmatter = []
    userspace = False
    cflags = None
    compname = None

    try:
        opts, args = getopt.getopt(sys.argv[1:], "f:luicpdjvo:h?",
                           ['frontmatter=', 'install', 'compile', 'preprocess', 'outfile=',
                            'document', 'help', 'userspace', 'cflags=', 'install-doc',
                            'compname=', 'view-doc', 'require-license', 'print-modinc'])
    except getopt.GetoptError:
        usage(1)

    for k, v in opts:
        if k in ("-u", "--userspace"):
            userspace = True
        if k in ("--cflags"):
            cflags = v
        if k in ("-i", "--install"):
            mode = INSTALL
        if k in ("-c", "--compile"):
            mode = COMPILE
        if k in ("--compname"):
            compname = v
        if k in ("-p", "--preprocess"):
            mode = PREPROCESS
        if k in ("-d", "--document"):
            mode = DOCUMENT
        if k in ("-j", "--install-doc"):
            mode = INSTALLDOC
        if k in ("-v", "--view-doc"):
            mode = VIEWDOC
        if k in ("--print-modinc",):
            mode = MODINC
        if k in ("-l", "--require-license"):
            require_license = True
        if k in ("-o", "--outfile"):
            if len(args) != 1:
                raise SystemExit("Error: Cannot specify -o with multiple input files")
            outfile = v
        if k in ("-f", "--frontmatter"):
            frontmatter.append(v)
        if k in ("-?", "-h", "--help"):
            usage(0)

    if outfile and mode != PREPROCESS and mode != DOCUMENT:
        raise SystemExit("Error: Can only specify -o when preprocessing or documenting")

    if mode == MODINC:
        if args:
            raise SystemExit(
                "Error: Can not specify input files when using --print-modinc")
        print (find_modinc())
        return 0

    c_sources = list()
    for f in args:
        try:
            basename = os.path.basename(os.path.splitext(f)[0])
            if f.endswith(".comp") and mode == DOCUMENT:
                adocument(f, outfile, frontmatter)
            elif f.endswith(".comp") and mode == VIEWDOC:
                tempdir = tempfile.mkdtemp()
                try:
                    outfile = os.path.join(tempdir, basename + ".asciidoc")
                    adocument(f, outfile, frontmatter)
                    cmd = "mank -f %s -p %s -s" % (basename, tempdir)
                    os.system(cmd)
                finally:
                    shutil.rmtree(tempdir)
            elif f.endswith(".comp") and mode == INSTALLDOC:
                manpath = os.path.join(BASE, "share/man/man9")
                if not os.path.isdir(manpath):
                    manpath = os.path.join(BASE, "man/man9")
                outfile = os.path.join(manpath, basename + ".asciidoc")
                print ("INSTALLDOC", outfile)
                adocument(f, outfile, frontmatter)
            elif f.endswith(".comp"):
                process(f, mode, outfile, cflags)
            elif f.endswith(".py") and mode == INSTALL:
                lines = open(f).readlines()
                if lines[0].startswith("#!"): del lines[0]
                lines[0] = "#!%s\n" % sys.executable
                outfile = os.path.join(BASE, "bin", basename)
                try: os.unlink(outfile)
                except os.error: pass
                open(outfile, "w").writelines(lines)
                os.chmod(outfile, stat.S_IREAD & stat.S_IEXEC)
            elif f.endswith(".c") and mode != PREPROCESS and not compname:
                initialize()
                tempdir = tempfile.mkdtemp()
                try:
                    shutil.copy(f, tempdir)
                    if userspace:
                        build_usr(tempdir, os.path.join(tempdir, os.path.basename(f)), mode, f, cflags)
                    else:
                        build_rt(tempdir, os.path.join(tempdir, os.path.basename(f)), mode, f, cflags)
                finally:
                    shutil.rmtree(tempdir)
            elif f.endswith(".c") and mode != PREPROCESS and compname:
                # Process all together later
                c_sources.append(f)
            else:
                raise SystemExit("Error: Unrecognized file type for mode %s: %r" % (modename[mode], f))
        except:
            ex_type, ex_value, exc_tb = sys.exc_info()
            try:
                os.unlink(outfile)
            except: # os.error:
                pass
            raise ex_type(ex_value, exc_tb)

    if not c_sources:
        return

    # Build C sources into single module
    initialize()
    tempdir = tempfile.mkdtemp()
    try:
        for f in c_sources:
            shutil.copy(f, tempdir)
        if userspace:
            build_usr(tempdir, os.path.join(tempdir, os.path.basename(f)),
                      mode, c_sources, cflags, compname)
        else:
            build_rt(tempdir, os.path.join(tempdir, os.path.basename(f)),
                     mode, c_sources, cflags, compname)
    finally:
        shutil.rmtree(tempdir)

if __name__ == '__main__':
    main()

# vim:sw=4:sts=4:et
