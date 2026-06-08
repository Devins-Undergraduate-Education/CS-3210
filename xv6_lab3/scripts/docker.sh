#!/usr/bin/env bash

function usage() {
    cat <<USAGE
    Usage: $0 [--attach] [--pull]

    Options:
        (default): create Docker image "xv6" and attach
        --attach: attach to created Docker image (useful for GDB)
        --pull: load latest Docker image from registry
USAGE
    exit 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
cd "${SCRIPT_DIR}/.."
ROOT_DIR="$(pwd)"

PULL=false
ATTACH=false
IMAGE="cs3210/xv6:latest"

for arg in "$@"; do
    case $arg in
    --pull)
        PULL=true
        shift
        ;;
    --attach)
        ATTACH=true
        shift
        ;;
    -h | --help)
        usage
        ;;
    *)
        usage
        ;;
    esac
done

# Function to check if Docker image exists locally
function image_exists() {
    docker image inspect "$IMAGE" > /dev/null 2>&1
}

# Auto-pull image if it doesn't exist
if ! image_exists; then
    echo "Image $IMAGE not found locally. Pulling from DockerHub..."
    docker pull "$IMAGE"
fi

# Handle options
if [[ $PULL == true ]]; then
    echo "Pulling latest image from DockerHub"
    docker pull "$IMAGE"
elif [[ $ATTACH == true ]]; then
    echo "Attaching to container"
    docker exec -it xv6 bash
else
    echo "Starting xv6 container"
    docker run --rm -it --name="xv6" -v "${ROOT_DIR}/":/xv6:z -w="/xv6" "$IMAGE"
fi
