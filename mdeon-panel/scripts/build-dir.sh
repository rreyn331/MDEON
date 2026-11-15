mdeon_panel_resolve_build_dir()
{
    root=$1
    build_dir=${MDEON_PANEL_BUILD_DIR:-${SIMPANEL_BUILD_DIR:-"$root/build"}}

    if [ -z "${MDEON_PANEL_BUILD_DIR:-}" ] && [ -z "${SIMPANEL_BUILD_DIR:-}" ] &&
        [ -f "$build_dir/CMakeCache.txt" ]; then
        cache_source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
            "$build_dir/CMakeCache.txt")
        if [ -n "$cache_source" ] && [ "$cache_source" != "$root" ]; then
            stale_cache_source=$cache_source
            build_key=$(printf '%s' "$root" | cksum)
            build_key=${build_key%% *}
            build_base="$root/build-$build_key"
            build_dir=$build_base
            suffix=1
            while [ -f "$build_dir/CMakeCache.txt" ]; do
                cache_source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
                    "$build_dir/CMakeCache.txt")
                if [ -z "$cache_source" ] || [ "$cache_source" = "$root" ]; then
                    break
                fi
                build_dir="$build_base-$suffix"
                suffix=$((suffix + 1))
            done
            printf '%s\n' \
                "Existing CMake cache belongs to $stale_cache_source; using $build_dir" >&2
        fi
    fi

    printf '%s\n' "$build_dir"
}