#!/usr/bin/env python3
import os
import sys
import argparse

# Get the directory where this script is located (i.e. ftp/)
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# Kconfig definition file (kept in ftp/, tracked by module git)
KCONFIG_PATH = os.path.join(SCRIPT_DIR, "Kconfig")

# Current working directory (CWD), i.e. project root directory
CWD = os.getcwd()

# Force bind configuration file (.ftp_config) in project root
CONFIG_FILE_PATH = os.path.join(CWD, ".ftp_config")

# Set environment variable to force kconfiglib and menuconfig to use .ftp_config.
# This prevents menuconfig from generating default .config and .config.old files on save.
os.environ["KCONFIG_CONFIG"] = CONFIG_FILE_PATH

def main():
    parser = argparse.ArgumentParser(description="FTP Kconfig Generator")
    parser.add_argument("-s", "--silent", action="store_true", help="Generate header without opening menuconfig")
    args = parser.parse_args()

    try:
        import kconfiglib
    except ImportError:
        print("Error: kconfiglib is not installed.")
        print("Please run: pip3 install kconfiglib")
        sys.exit(1)

    if not os.path.exists(KCONFIG_PATH):
        print(f"Error: Kconfig not found at '{KCONFIG_PATH}'")
        sys.exit(1)

    # Load Kconfig rules (reads os.environ['KCONFIG_CONFIG'] automatically)
    kconf = kconfiglib.Kconfig(KCONFIG_PATH)

    # Open menuconfig GUI by default unless --silent is specified
    if not args.silent:
        try:
            from menuconfig import menuconfig
            menuconfig(kconf)
            # Force a sync write after menuconfig GUI closes
            kconf.write_config(CONFIG_FILE_PATH)
        except ImportError:
            print("Error: menuconfig library interface not found.")
            sys.exit(1)
    else:
        # Silent mode: load existing config or write default if not present
        if os.path.exists(CONFIG_FILE_PATH):
            kconf.load_config(CONFIG_FILE_PATH)
        else:
            kconf.write_config(CONFIG_FILE_PATH)

    # Get output directory configuration option from Kconfig
    header_dir_sym = kconf.syms.get("FTP_CONFIG_HEADER_DIR")
    if header_dir_sym and header_dir_sym.str_value:
        target_dir = header_dir_sym.str_value
    else:
        target_dir = "."

    # Resolve target directory path relative to CWD
    abs_target_dir = os.path.abspath(os.path.join(CWD, target_dir))

    # Ensure the target directory exists
    if not os.path.exists(abs_target_dir):
        os.makedirs(abs_target_dir, exist_ok=True)

    header_file = os.path.join(abs_target_dir, "ftp_config.h")

    # Generate C header file
    write_header(kconf, header_file)
    print(f"Successfully generated FTP configuration at: {header_file}")

def write_header(kconf, filepath):
    import kconfiglib
    with open(filepath, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated configuration. Do not edit. */\n")
        f.write("#ifndef _ftp_config_h_\n")
        f.write("#define _ftp_config_h_\n\n")

        for sym in kconf.unique_defined_syms:
            if sym.type == kconfiglib.UNKNOWN:
                continue

            name = sym.name

            # Filter out the directory configuration variable, do not write it to C header
            if name == "FTP_CONFIG_HEADER_DIR":
                continue

            val = sym.str_value
            if sym.type == kconfiglib.BOOL:
                f.write(f"#define CONFIG_{name} {1 if val == 'y' else 0}\n")
            elif sym.type in (kconfiglib.INT, kconfiglib.HEX):
                f.write(f"#define CONFIG_{name} {val}\n")
            elif sym.type == kconfiglib.STRING:
                f.write(f"#define CONFIG_{name} \"{val}\"\n")

        f.write("\n#endif\n")


if __name__ == "__main__":
    main()
