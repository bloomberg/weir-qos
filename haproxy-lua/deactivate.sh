#!/usr/bin/bash

set -e

# Enable ** for directory expansion in globs, and allow zero matches to result in an empty list
shopt -s globstar nullglob

SCRIPT_DIR=$(dirname "$0")

if [[ ! -d ./haproxy-source ]]; then
    echo "Haproxy source directory does not exist, did you run activate.sh?"
    exit 1
fi

if [[ ! -f ./.haproxy-activated-commit ]]; then
    echo "Base commit not set, did you run activate.sh?"
    exit 1
fi

# Verify that all added files exist in the repository before we start making changes.
# This helps avoid us getting into a half-deactivated state.
for addedfile in "$SCRIPT_DIR"/added-files/**/*.*; do
    if [[ ! -f "./haproxy-source/${addedfile#"$SCRIPT_DIR"/added-files/}" ]]; then
        echo "Added file ${addedfile} is not present in the repository, deactivation cancelled to avoid data loss"
        exit 1
    fi
done

WEIR_HAPROXY_BASE_COMMIT=$(cat ./.haproxy-activated-commit)
rm ./.haproxy-activated-commit

# Remove existing patches so that we don't get leftover unexpected patches if we change a commit message.
# Force so we don't fail if there aren't any or they've already been deleted somehow
rm --force ./patches/*

# Move added-files out of the repo. This both updates our added-files and also ensures
# that they don't form part of the patch set that we're about to generate.
for addedfile in "$SCRIPT_DIR"/added-files/**/*.*; do
    echo "Moving $addedfile out of the haproxy source tree..."
    mv "./haproxy-source/${addedfile#"$SCRIPT_DIR"/added-files/}" "$addedfile"
done

# Ensure there are no uncommitted changes. This serves 3 functions:
# 1. It ensures we've not accidentally missed any changes that should be in a patch after deactivation.
# 2. It leaves us with a clean-slate, ready for us to call activate.sh later next time we're ready to make changes.
# 3. It verifies that we've not accidentally introduced a new file in a patch (which should instead have been added
#    in the added-files directory. If a new file gets added in a commit and also added in added-files, then after moving
#    out above, it will show up in the haproxy repo as a removed file.
if (! git -C ./haproxy-source diff --quiet) || (! git -C ./haproxy-source diff --staged --quiet); then
    echo "There are outstanding changes in the haproxy source code, cancelling deactivation"
    echo "To add a new file, do not commit it in a patch to haproxy, instead touch the file path in added-files before deactivating."
    exit 1
fi

echo "Formatting patches starting at $WEIR_HAPROXY_BASE_COMMIT"
git -C ./haproxy-source format-patch --output-directory ../patches --zero-commit --no-numbered "${WEIR_HAPROXY_BASE_COMMIT}...HEAD"

echo "Deactivation complete"
