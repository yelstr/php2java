echo "Cleaning php2java object files ..."
find ./ -name "*.lo" -o -name "*.o" | xargs rm -f
rm -f php2java