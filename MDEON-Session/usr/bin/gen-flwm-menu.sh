#!/bin/bash
# Script to generate .wmx menu for flwm
# Requires: bash, find, sed

WMXDIR="$HOME/.wmx"
APPSDIR="/usr/share/applications"

# 1. Clean up old menu
rm -rf "$WMXDIR"
mkdir -p "$WMXDIR"

# 2. Iterate through .desktop files
find "$APPSDIR" -name "*.desktop" -print0 | while IFS= read -r -d '' file; do 
    # Skip if contains exo-open
    if grep -q "exo-open" "$file"; then continue; fi
    
    # Extract App Name (first occurrence only)
    name=$(grep "^Name=" "$file" | head -n1 | cut -d'=' -f2-)
    
    # Extract Exec command (first occurrence only)
    exec=$(grep "^Exec=" "$file" | head -n1 | cut -d'=' -f2- | sed 's/%.//g')
    
    # Extract Categories
    categories=$(grep "^Categories=" "$file" | cut -d'=' -f2-)
    
    # Skip if no name, exec, or categories found
    if [[ -z "$name" || -z "$exec" || -z "$categories" ]]; then continue; fi

    # Create category directory structure
    # Use first category from the list
    category=$(echo "$categories" | cut -d';' -f1)
    category_dir="$WMXDIR/$category"
    mkdir -p "$category_dir"
    
    # Sanitizing name for filename
    safe_name=$(echo "$name" | tr -d '[:cntrl:]' | sed 's/[^a-zA-Z0-9._-]//g')
    
    # Create wrapper script in category directory
    cat <<EOF > "$category_dir/$safe_name"
#!/bin/sh
$exec &
EOF
    chmod +x "$category_dir/$safe_name"
done

# 3. Add System menu with Logout, Shutdown, Restart, and Reload Menu options
mkdir -p "$WMXDIR/Z-System"

echo "#!/bin/sh" > "$WMXDIR/Z-System/Logout"
echo "flwm -x &" >> "$WMXDIR/Z-System/Logout"
chmod +x "$WMXDIR/Z-System/Logout"

echo "#!/bin/sh" > "$WMXDIR/Z-System/Shutdown"
echo "shutdown -h now &" >> "$WMXDIR/Z-System/Shutdown"
chmod +x "$WMXDIR/Z-System/Shutdown"

echo "#!/bin/sh" > "$WMXDIR/Z-System/Restart"
echo "shutdown -r now &" >> "$WMXDIR/Z-System/Restart"
chmod +x "$WMXDIR/Z-System/Restart"

echo "#!/bin/sh" > "$WMXDIR/Z-System/Reload_Menu"
echo "/usr/bin/gen-flwm-menu.sh &" >> "$WMXDIR/Z-System/Reload_Menu"
chmod +x "$WMXDIR/Z-System/Reload_Menu"

echo "FLWM menu generated in $WMXDIR"
