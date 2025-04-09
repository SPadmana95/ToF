#!/bin/bash

DESTINATION="./output"
RESOURCES="./adcam-installer/resources"
SRC_PREFIX="../../build"
EVALUATION="$DESTINATION/eval"
TOOLS="$DESTINATION/tools"
DEVELOPMENT="$DESTINATION/dev"
KERNEL="$DESTINATION/kernel"
LIBS="$DESTINATION/libs"
LIB_INSTALL_FOLDER="/tmp/ADI-ADCAM" # TODO: Get rid of this?
GIT_CLONE_SCRIPT_NAME="$DEVELOPMENT/git_clone_tof.sh"
RUNME_SCRIPT_NAME="$DESTINATION/run_me.sh"
COPY_LOG="/tmp/copied_files.txt"
CONFIG_JSON="./config.json"
BRANCH=$(git rev-parse --abbrev-ref HEAD)
CMAKE_LOGS="/tmp/cmake_errors.log"
JETPACK_VERSION="Unknown"
VERSION="Unknown"
FORCE=false
UPDATE=false
BUILDALL=false
COMPRESS=false
REMOTE_KERNEL_BUILD=false
WITH_NETWORK=OFF
original_ld_library_path="$LD_LIBRARY_PATH"

# Parse arguments
print_help() {
    echo "🔧 Usage: $0 [-h][-f][-u][-a][-c][-n][-r]"
    echo ""
    echo "  -f    Force rebuild by deleting the existing build directory"
    echo "  -u    Update the Ubuntu dependencies"
    echo "  -n    Build WITH_NETWORK=ON"
    echo "  -a    Build installation package to include SDK and Kernel components" 
    echo "  -r <user>,<ip>   Build kernel remotely" 
    echo "  -c    Compress Rust generated binary."
    echo "  -h    Show this help message"
}

# Parse options
while getopts ":fhuar:c" opt; do
  case $opt in
    f) FORCE=true ;;
    u) UPDATE=true ;;
    a) BUILDALL=true ;;
    c) COMPRESS=true ;;
    n) WITH_NETWORK=ON ;;
    r)
      REMOTE_KERNEL_BUILD=true
      IFS=',' read -r REMOTE_USER REMOTE_IP <<< "$OPTARG" # Split the argument
      ;;
    h) print_help; exit 0 ;;
    \?) echo "❌ Invalid option: -$OPTARG" >&2; print_help; exit 1 ;;
    :) echo "❌ Option -$OPTARG requires an argument." >&2; print_help; exit 1 ;;
  esac
done

shift $((OPTIND - 1))

if $REMOTE_KERNEL_BUILD; then
  if [ -z "$REMOTE_USER" ] || [ -z "$REMOTE_IP" ]; then
    echo "❌ Error: -r option requires a username and IP address (e.g., -r user,192.168.1.1)." >&2
    print_help
    exit 1
  fi
  echo "Building kernel remotely as user: $REMOTE_USER on IP: $REMOTE_IP"
  # ... your remote build commands here ...
else
  echo "Building kernel locally."
  # ... your local build commands here ...
fi

confim_yes_or_no() {

    read -p "Are you sure? (yes/no): " confirmation

    # Convert to lowercase for case-insensitive comparison
    confirmation=$(echo "$confirmation" | tr '[:upper:]' '[:lower:]')

    if [[ "$confirmation" == "yes" ]]; then
        echo "Confirmed. Proceeding..."
    else
        echo "Terminating process, exiting."
        exit
    fi
}

clean_folder_contents() {
    local target_dir="$1"

    if [ -z "$target_dir" ]; then
        echo "❌ Usage: clean_folder_contents <target_dir>"
        return 1
    fi

    if [ ! -d "$target_dir" ]; then
        echo "📁 Creating directory: $target_dir"
        mkdir -p "$target_dir"
    fi

    if [ -d "$target_dir" ]; then
        echo "🧹 Cleaning all contents inside: $target_dir"
        find "$target_dir" -mindepth 1 -delete
        echo "✅ Folder cleaned!"
    else
        echo "❌ Failed to access directory: $target_dir"
        return 1
    fi

    rmdir "$target_dir"
}

#########################
# Extract the versions
#########################
get_sdk_version() {

    CMAKE_FILE="../../CMakeLists.txt"

    if [ ! -f "$CMAKE_FILE" ]; then
        echo "Cannot find the CMakeLists.txt file."
        exit 1
    fi

    # Extract version components
    MAJOR=$(grep -oP 'set\s*\(\s*ADITOF_VERSION_MAJOR\s+\K[0-9]+' "$CMAKE_FILE")
    MINOR=$(grep -oP 'set\s*\(\s*ADITOF_VERSION_MINOR\s+\K[0-9]+' "$CMAKE_FILE")
    PATCH=$(grep -oP 'set\s*\(\s*ADITOF_VERSION_PATCH\s+\K[0-9]+' "$CMAKE_FILE")
    # Combine into full version
    VERSION="$MAJOR.$MINOR.$PATCH"

    echo "📦 Extracted ADITOF version: $VERSION"
}

get_jetpack_version() {

    JETPACK_FILE="/etc/nv_tegra_release"
    JETPACK_VERSION="unknown"

    if [ -f "$JETPACK_FILE" ]; then
        # Extract the L4T major version (e.g., R36)
        L4T_MAJOR=$(grep -oP 'R\d+' "$JETPACK_FILE" | head -n 1 | tr -d 'R')

        # Extract the L4T minor version (e.g., 4.3)
        L4T_REVISION=$(grep "REVISION:" "$JETPACK_FILE" | sed -n 's/.*REVISION: \([^,]*\).*/\1/p')

        FULL_L4T="${L4T_MAJOR}.${L4T_REVISION}"

        echo "$FULL_L4T"
        case "$FULL_L4T" in
            36.4.3) JETPACK_VERSION="JP62" ;;
            *)      
                    echo "Unsupported JetPack version, terminating script."
                    exit 1
                    ;;
        esac

        echo "🔍 Detected JetPack version: $JETPACK_VERSION"
    else
        echo "❌ $JETPACK_FILE not found. Are you on a Jetson device?"
        exit 1
    fi

    if [ ! -f "$CONFIG_JSON" ]; then
        echo "❌ JSON file not found: $CONFIG_JSON"
        exit 1
    fi

    # Use jq to add or update "jetpack"
    jq --arg ver "$JETPACK_VERSION" '.jetpack = $ver' "$CONFIG_JSON" > tmp.json && mv tmp.json "$CONFIG_JSON"

    echo "✅ jetpack version set to: $JETPACK_VERSION in $CONFIG_JSON"

    echo "$JETPACK_VERSION"
}

build_eval_kit() {

    rm -rf "$LIB_INSTALL_FOLDER"
    mkdir "$LIB_INSTALL_FOLDER"
    # Only remove if the folder exists
    if [ "$FORCE" = true ]; then
        echo "Removing existing build directory: $SRC_PREFIX"
        if [ -d "$SRC_PREFIX" ]; then
            rm -rf "$SRC_PREFIX"
        fi
    else
        echo "No existing build directory to remove."
    fi

    # Update LD_LIBRARY_PATH (add a new directory) # TODO: Why is this needed??
    new_library_directory=$(realpath "$SRC_PREFIX")/libaditof/protobuf/cmake
    LD_LIBRARY_PATH="$new_library_directory:$LD_LIBRARY_PATH"

    # Run CMake to generate build system
    echo "Generating build files in: $SRC_PREFIX"
    cmake -DCMAKE_INSTALL_PREFIX="$LIB_INSTALL_FOLDER" \
        -DCMAKE_INSTALL_RPATH="$LIB_INSTALL_FOLDER"\lib \
        -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
        -DON_NVIDIA=ON \
        -DWITH_NETWORK="$WITH_NETWORK" \
        -S ../../ \
        -B "$SRC_PREFIX" 2> "$CMAKE_LOGS"
    if [ $? -ne 0 ]; then
        echo "cmake configuration failed: $LINENO"
        echo "Cmake Log"
        echo "---------"
        cat "$CMAKE_LOGS"
        exit
    fi

    cmake --build "$SRC_PREFIX" 2> "$CMAKE_LOGS"
    if [ $? -ne 0 ]; then
        echo "cmake build failed: $LINENO"
        echo "Cmake Log"
        echo "---------"
        cat "$CMAKE_LOGS"
        exit
    fi

    cmake --install "$SRC_PREFIX" 2> "$CMAKE_LOGS"
    if [ $? -ne 0 ]; then
        echo "cmake install failed: $LINENO"
        echo "Cmake Log"
        echo "---------"
        cat "$CMAKE_LOGS"
        exit
    fi

    if [ -d "./CMakeFiles" ]; then
        echo "Cleaning up CMake-generated files in: $SRC_PREFIX"

        rm -rf "./CMakeFiles"
    fi 

    # Check if Rust is installed
    if command -v rustc >/dev/null 2>&1; then
        echo "✅ Rust is already installed (version $(rustc --version))"
    else
        echo "🛠 Installing Rust for ARM64..."

        # Download and run rustup-init
        curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain stable

        # Add Cargo to PATH for current shell session
        export PATH="$HOME/.cargo/bin:$PATH"

        echo "✅ Rust installed (version $(rustc --version))"
    fi
}

#########################
### Setup staging folder
#########################
setup_staging() {

    # Clean staging folder
    clean_folder_contents "$EVALUATION"

    # ✨ Clear log file
    > "$COPY_LOG"


    source ./extra/filelist.sh

    for entry in "${COPY_PATTERNS[@]}"; do
        IFS=',' read -r OP REL_SRC_PATTERN REL_DST_DIR <<< "$entry"

        OPERATION="$OP"
        SRC_PATTERN="$REL_SRC_PATTERN"
        DST_DIR="$REL_DST_DIR"

        echo "Processing: '$SRC_PATTERN' → '$DST_DIR'"

        # Create destination directory
        if [[ "$DST_DIR" == /opt/* || "$DST_DIR" == /usr/* ]]; then
            echo "❌ Destination directory is not writable: $DST_DIR"
            exit
        else
            mkdir -p "$DST_DIR"
        fi

        for file in $SRC_PATTERN; do
            if [ -e "$file" ]; then
                echo "  ↪ Copying $file → $DST_DIR"

                OP_FAILED=true
                if [ "$OPERATION" = "mv" ]; then
                    mv "$file" "$DST_DIR/"
                    OP_FAILED=false
                else
                    cp -r "$file" "$DST_DIR/"
                    OP_FAILED=false
                fi

                if [ "$OP_FAILED" = true ]; then
                    echo "❌ Operation failed: $entry"
                    exit
                fi

                # Determine how to log files based on type
                if [ -d "$file" ]; then
                    find "$file" -type f | while read -r subfile; do
                        rel_path="${subfile#$file/}"                    # relative to the source dir
                        dest_path="$DST_DIR/$(basename "$file")/$rel_path"
                        echo "$dest_path" >> "$COPY_LOG"
                    done
                else
                    dest_path="$DST_DIR/$(basename "$file")"
                    echo "$dest_path" >> "$COPY_LOG"
                fi
            else
                echo "  ⚠️  No match for: $file"
            fi
        done
    done

    rm -rf "$LIB_INSTALL_FOLDER"

    echo "✅ Done copying files!"
    echo "📄 File list saved to: $COPY_LOG"
}

#########################
# Build the kernel components
## It may be useful to build the kernel components separately
## on a different machine using cross-compilation.
## Add an option to do this
#########################
build_kernel() {
    if [ "$BUILDALL" = true ]; then

        if [ "$REMOTE_KERNEL_BUILD" == true ]; then
            RANDOM_DIRECTORY=/tmp/"$RANDOM"

            echo "$REMOTE_USER" "$REMOTE_IP" "$RANDOM_DIRECTORY"

            # Create directory on remote host
            ssh "$REMOTE_USER"@"$REMOTE_IP" "mkdir -p $RANDOM_DIRECTORY"
            if [ "$?" -ne 0 ]; then
                echo "❌ Unable to create remote directory, remote build failed, starting local build."
                exit
            fi

            # Copy script to remove machine
            scp ./extra/remote_kernel_build.sh "$REMOTE_USER"@"$REMOTE_IP":"$RANDOM_DIRECTORY"
            if [ "$?" -ne 0 ]; then
                echo "❌ Unable to copy file to remote server, remote build failed, starting local build."
                exit
            fi 
            echo "✅ Remote kernel build script copied successfully."
        
            # Execute remote script
            # TODO: Execute in the background
            ssh "$REMOTE_USER"@"$REMOTE_IP" "cd $RANDOM_DIRECTORY && source ./remote_kernel_build.sh $VERSION $BRANCH"
            if [ "$?" -ne 0 ]; then
                echo "❌ Remote kernel build failed, remote build failed, starting local build."
                exit
            fi

            # Copy script to remove machine
            scp "$REMOTE_USER"@"$REMOTE_IP":"$RANDOM_DIRECTORY"/NVIDIA_ToF_ADSD3500_REL_PATCH.zip "./extra/NVIDIA_ToF_ADSD3500_REL_PATCH.zip"
            if [ "$?" -ne 0 ]; then
                echo "❌ Failed to copy the kernel build from remote machine, remote build failed, starting local build."
                exit
            fi 

            echo "✅ Remote kernel build completed."
        else
            DRIVER_FOLDER=../../sdcard-images-utils/nvidia
            pushd .
            cd "$DRIVER_FOLDER"
            "$DRIVER_FOLDER"/runme.sh "$VERSION" "$BRANCH"
            if [ -f ./build ]; then
                rm -rf ./build
            fi
            popd 

            MATCH=$(compgen -G "${DRIVER_FOLDER}/NVIDIA_ToF_ADSD3500_REL_PATCH_*.zip" | head -n 1)

            if [ -n "$MATCH" ]; then
                echo "✅ Building with kernel bits: $MATCH"
                cp "$MATCH" ./extra/NVIDIA_ToF_ADSD3500_REL_PATCH.zip
            else
                echo "⚠️ No matching .zip file found - Building without the Kernel bits."
                confim_yes_or_no
            fi
        fi
    fi

    if [ -f "$KERNEL/NVIDIA_ToF_ADSD3500_REL_PATCH.zip" ]; then
        # Backup kernel image for later use if needed
        echo "Backuping up NVIDIA_ToF_ADSD3500_REL_PATCH.zip for later use, if needed."
        cp "$KERNEL/NVIDIA_ToF_ADSD3500_REL_PATCH.zip" ./extra/
    else
        # Use backup if avaialble
        if [ -f "./extra/NVIDIA_ToF_ADSD3500_REL_PATCH.zip" ]; then
            echo "Attempting to use backup kernel."
            cp ./extra/NVIDIA_ToF_ADSD3500_REL_PATCH.zip "$KERNEL/NVIDIA_ToF_ADSD3500_REL_PATCH.zip"
        fi
    fi

    if [ ! -f "$KERNEL/NVIDIA_ToF_ADSD3500_REL_PATCH.zip" ]; then
            echo "❌ No kernel build found, exiting build."
            exit
    else
        echo "Unpacking NVIDIA_ToF_ADSD3500_REL_PATCH.zip"
        pushd .
        cd "$KERNEL"
        unzip -o NVIDIA_ToF_ADSD3500_REL_PATCH.zip
        rm NVIDIA_ToF_ADSD3500_REL_PATCH.zip
        popd
    fi
}

#########################
# Create Git clone script in the staging folder for the current branch
#########################
build_git_clone_sh() {
    # Write the embedded script to disk
    echo "#!/bin/bash" > "$GIT_CLONE_SCRIPT_NAME"
    echo '' >> "$GIT_CLONE_SCRIPT_NAME"
    echo set -e >> "$GIT_CLONE_SCRIPT_NAME"
    echo '' >> "$GIT_CLONE_SCRIPT_NAME"
    echo DEFAULT_VERSION="$BRANCH" >> "$GIT_CLONE_SCRIPT_NAME"
    echo VERSION=\"\${1:-\$DEFAULT_VERSION}\" >> "$GIT_CLONE_SCRIPT_NAME"
    echo '' >> "$GIT_CLONE_SCRIPT_NAME"
    echo echo "Cloning branch $VERSION from ToF repo..." >> "$GIT_CLONE_SCRIPT_NAME"
    echo git clone https://github.com/analogdevicesinc/ToF ToF-\"\$VERSION\" >> "$GIT_CLONE_SCRIPT_NAME"
    echo cd ToF-\"\$VERSION\" >> "$GIT_CLONE_SCRIPT_NAME"
    echo git submodule update --init >> "$GIT_CLONE_SCRIPT_NAME"
    echo git checkout \"\$VERSION\" >> "$GIT_CLONE_SCRIPT_NAME"
    echo cd libaditof >> "$GIT_CLONE_SCRIPT_NAME"
    echo git checkout \"\$VERSION\" >> "$GIT_CLONE_SCRIPT_NAME"
    echo cd .. >> "$GIT_CLONE_SCRIPT_NAME"
    echo '' >> "$GIT_CLONE_SCRIPT_NAME"
    echo echo "✅ Done!" >> "$GIT_CLONE_SCRIPT_NAME"


    # Make it executable
    chmod +x "$GIT_CLONE_SCRIPT_NAME"

    echo "$GIT_CLONE_SCRIPT_NAME" >> "$COPY_LOG"

    echo "Created script: $GIT_CLONE_SCRIPT_NAME"
}

#########################
# Create script to point to library files
#########################
build_run_me_sh() {
    # TODO:  Make this script install the kernel patch - if needed

    echo '#!/bin/bash' > "$RUNME_SCRIPT_NAME"
    echo '' >> "$RUNME_SCRIPT_NAME"
    cat extra/add_on_apply_kernel_patch.sh >> "$RUNME_SCRIPT_NAME"
    echo '' >> "$RUNME_SCRIPT_NAME"
    cat extra/add_on_check_packages.sh >> "$RUNME_SCRIPT_NAME"
    echo '' >> "$RUNME_SCRIPT_NAME"
    echo LIBS_PATH=\$\(realpath \"libs\"\) >> "$RUNME_SCRIPT_NAME"
    echo echo \"Creating Directory Path Soft Link: \$LIBS_PATH\" >> "$RUNME_SCRIPT_NAME"
    echo '' >> "$RUNME_SCRIPT_NAME"
    echo echo \"\$LIBS_PATH/lib\" \| sudo tee /etc/ld.so.conf.d/adi-adcam.conf >> "$RUNME_SCRIPT_NAME"
    echo sudo ldconfig >> "$RUNME_SCRIPT_NAME"

    if [ ! -f "$RUNME_SCRIPT_NAME" ]; then
        echo "❌ Stage file not created: $RUNME_SCRIPT_NAME"
        return -1
    fi

    chmod +x "$RUNME_SCRIPT_NAME"

    echo "$RUNME_SCRIPT_NAME" >> "$COPY_LOG"

    echo "Created script: $RUNME_SCRIPT_NAME"
}

#########################
# Create permissions.json
#########################
create_permissions_json() {
    set -e

    PERMISSIONS_FILE="$RESOURCES/permissions.json"

    # Ensure jq is installed
    if ! command -v jq &>/dev/null; then
        echo "❌ jq is required but not installed."
        exit 1
    fi

    # Initialize empty JSON object
    echo "{}" > "$PERMISSIONS_FILE"

    # Walk through all files under the directory
    find "$DESTINATION" -type f | while read -r file; do
        # Strip the DESTINATION prefix
        rel_path="${file#$DESTINATION/}"

        # Get file permissions (mode) in octal
        mode=$(stat -c "%a" "$file")

        # Add entry to JSON using jq
        tmp=$(mktemp)
        jq --arg key "$rel_path" --arg value "$mode" '. + {($key): ($value | tonumber)}' "$PERMISSIONS_FILE" > "$tmp" && mv "$tmp" "$PERMISSIONS_FILE"
    done

    echo "✅ Permissions written to: $PERMISSIONS_FILE"
}

#########################
# Build the installer
#########################
build_installer() {
    # Create output.zip
    STAGING_FILE=$(realpath install-bundle.tgz)
    tar -czf "$STAGING_FILE" -C "$DESTINATION/" .

    if [ ! -f "$STAGING_FILE" ]; then
        echo "❌ Stage file not created: $STAGING_FILE"
        return -1
    fi

    # Build final package
    echo "Starting build of installer binary: "
    make -C ./adcam-installer clean
    make_output=$(make -C ./adcam-installer package JETPACKVERSION="$JETPACK_VERSION" BUNDLE="$STAGING_FILE")
    installer_path=$(echo "$make_output" | grep "^BuiltXYZ: " | awk '{print $2}')
    installer_path=./adcam-installer/"$installer_path"
    if [ "$COMPRESS" = true ]; then
        echo "✅ Compressing $installer_path"
        upx --best --lzma "$installer_path"
    else
        echo "⚠️ Not compressing $installer_path"
    fi

    return 0
}

#########################
# Build the documentation
#########################
build_documentation() {
    echo For building the documentation
}

#########################
# Final clean up
#########################
clean_up() {
    echo "✅ Cleaning up on exit!"
    LD_LIBRARY_PATH="$original_ld_library_path"
    clean_folder_contents "$DESTINATION"
    rm "$STAGING_FILE"
    rm "$COPY_LOG"
    rm "$CMAKE_LOGS"
}

main() {
    get_sdk_version
    get_jetpack_version

    # Created needed folders
    mkdir -p "$EVALUATION"
    mkdir -p "$TOOLS"
    mkdir -p "$DEVELOPMENT"
    mkdir -p "$KERNEL"

    build_kernel
    build_eval_kit
    setup_staging
    build_git_clone_sh
    build_run_me_sh
    build_documentation
    create_permissions_json
    build_installer
}

trap clean_up EXIT
main