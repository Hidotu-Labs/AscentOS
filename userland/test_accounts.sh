#!/bin/sh
set -eu

user=phase4test
group=phase4shared
home=/home/$user

userdel -r "$user" >/dev/null 2>&1 || true
groupdel "$group" >/dev/null 2>&1 || true

groupadd -g 2100 "$group"
useradd -m -u 2101 -s /bin/bash "$user"
usermod -aG "$group" "$user"

grep -q "^$user:!:2101:" /etc/passwd
grep -q "^$group:x:2100:$user" /etc/group
[ "$(stat -c %u "$home")" = 2101 ]
[ "$(stat -c %a "$home")" = 700 ]
[ ! -e /etc/shadow ]

printf "%s:%s\n" "$user" ascent-test-password | chpasswd -c SHA512
grep -Fq "$user:\$6\$" /etc/passwd
su - "$user" -c "test \"\$(id -u)\" = 2101"

userdel -r "$user"
groupdel "$group"
echo "Phase 4 account tools: PASS"
