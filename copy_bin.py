Import("env")
import os
import shutil

def copy_binaries(source, target, env):
    print(">>> Running Post-Build script: Copying binaries to ESPWebKitS3...")

    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    dest_dir = os.path.join(project_dir, "ESPWebKitS3")

    # Ensure the destination directory exists
    os.makedirs(dest_dir, exist_ok=True)

    # Define the mapping of source files to destination names
    files_to_copy = {
        "bootloader.bin": "01_bootloader.bin",
        "partitions.bin": "02_partitions.bin",
        "boot_app0.bin":  "03_boot_app0.bin",
        "firmware.bin":   "04_firmware.bin",
        "littlefs.bin":   "05_filesystem.bin" 
    }

    for src_name, dest_name in files_to_copy.items():
        src_path = os.path.join(build_dir, src_name)
        
        # Special handling for boot_app0.bin 
        # (If not generated in the build directory, locate it in the framework tools)
        if src_name == "boot_app0.bin" and not os.path.exists(src_path):
            framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
            if framework_dir:
                alt_path = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")
                if os.path.exists(alt_path):
                    src_path = alt_path

        dest_path = os.path.join(dest_dir, dest_name)

        # Execute the copy process
        if os.path.exists(src_path):
            shutil.copy(src_path, dest_path)
            print(f"  [+] Successfully copied: {src_name} -> {dest_name}")
        else:
            print(f"  [-] Warning: {src_name} not found, skipping.")

# Bind this action to be executed after the firmware (.bin) is built
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_binaries)
