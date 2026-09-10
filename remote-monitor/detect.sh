#!/system/bin/sh
for d in /data/adb /data/local /data/local/tmp /cache /data /dev /data/misc /mnt/sdcard /sbin; do
  echo "=== $d ==="
  if touch "$d/.w" 2>/dev/null; then
    echo "write:OK"; rm -f "$d/.w"
  else
    echo "write:DENIED"
  fi
  printf '#!/system/bin/sh\necho EXEC_OK\n' > "$d/.t" 2>/dev/null
  if chmod 755 "$d/.t" 2>/dev/null && "$d/.t" 2>/dev/null; then
    echo "exec:OK"
  else
    echo "exec:DENIED"
  fi
  rm -f "$d/.t"
done
echo "--- mounts (relevant) ---"
mount | grep -E "local/tmp|/data/adb| /data |/cache" | head -20
