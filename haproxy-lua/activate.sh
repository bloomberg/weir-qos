#!/usr/bin/bash

set -e

WEIR_HAPROXY_BASE_COMMIT=v3.2.0
SCRIPT_DIR=$(dirname "$0")
HAPROXY_SOURCE_DIR=$SCRIPT_DIR/haproxy-source

# Clone haproxy from the upstream repo, if it doesn't already exist
if [[ -d "$HAPROXY_SOURCE_DIR" ]]; then
    echo "HAProxy directory already exists @ $HAPROXY_SOURCE_DIR, skipping clone step..."
else
    git clone "${WEIR_HAPROXY_REPO_URL:-https://github.com/haproxy/haproxy.git}" "$HAPROXY_SOURCE_DIR"
fi

if (! git -C  "$HAPROXY_SOURCE_DIR" diff --quiet) || (! git -C  "$HAPROXY_SOURCE_DIR" diff --staged --quiet); then
    echo "There are existing changes in the haproxy source code, cancelling activation to avoid data loss"
    exit 1
fi

# Store the commit on which our local changes are based, so that we know which commits need to be
# turned into patches when we later run the `deactivate` script.
git -C  "$HAPROXY_SOURCE_DIR" checkout $WEIR_HAPROXY_BASE_COMMIT
git -C  "$HAPROXY_SOURCE_DIR" show-ref -s $WEIR_HAPROXY_BASE_COMMIT > "$SCRIPT_DIR"/.haproxy-activated-commit

# If there is no username and email configured on the git repo, configure an example one locally
# so that we can safely apply the haproxy patches below.
if ! git -C "$HAPROXY_SOURCE_DIR" config --get user.name; then
    git -C "$HAPROXY_SOURCE_DIR" config --local user.name HAProxyBuild
    git -C "$HAPROXY_SOURCE_DIR" config --local user.email haproxybuild@example.com
fi

# Apply our set of patches.
# We specifically *do* want the realpath output to be split on the
# line below so that git will apply all the patches for us.
# shellcheck disable=SC2046
git -C "$HAPROXY_SOURCE_DIR" am  $(realpath "$SCRIPT_DIR"/patches/*)


# Enable ** for directory expansion in globs, and allow zero matches to result in an empty list
shopt -s globstar nullglob

# Copy into the repo any entirely new files that we've added.
# These are tracked here instead of as part of the patch files simply because reviewing changes
# to the patch files in this repo is much more painful and difficult than reviewing changes to
# files located directly in this repo. We still need patches for a few minor modifications but
# the overwhelming majority of our changes should be to newly-added files, making reviews just
# as easy as for any other change.
# We do this *before* applying patches so that if there is a conflict when applying those patches
# (as there could be when upgrading the base version of haproxy), then once the failed patches
# have been manually applied, the source directory will be in the correct fully-activated state
# and can simply be deactivated again to get the updated patches out.
for addedfile in "$SCRIPT_DIR"/added-files/**/*.*; do
    echo "Copying $addedfile to the haproxy source tree..."
    cp "$addedfile" "$HAPROXY_SOURCE_DIR/${addedfile#"$SCRIPT_DIR"/added-files}"
done

echo "Activation complete"
