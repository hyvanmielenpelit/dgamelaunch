#!/bin/bash

if [ $# -eq 0 ]; then
  echo "No arguments. The first argument must be the name of the GnollHack directory. For example, ghn5."
  exit 1
fi

if [ $# -eq 1 ]; then
  echo "Only one argument. The second argument must be the short name of the GnollHack game. For example, gnollhack5."
  exit 1
fi

if test -f /$1/gnollhack; then
  echo "GnollHack found in /$1/"
else
  echo "GnollHack not found in /$1/. Exiting."
  exit 1
fi

echo "Copying Banner Files examples/*.txt"
sudo cp examples/*.txt /opt/gnollhack/server.gnollhack.com/

echo "Copying dgamelaunch.conf"
sudo cp examples/dgamelaunch.conf /opt/gnollhack/server.gnollhack.com/etc

if [ $# -eq 3 ]; then
  echo "Modifying the server address in dgamelaunch.conf to $3-server.gnollhack.com"
  sudo sed -i -r "s/ATTR\(14\)server.gnollhack.com/ATTR\(14\)$3-server.gnollhack.com/g" /opt/gnollhack/server.gnollhack.com/etc/dgamelaunch.conf
else
  echo "The server address in dgamelaunch.conf was not modified, since the second argument was not given."
  echo "If you want to change the server address automatically, the second argument needs to be, for example, 'eu'."
fi

if [ -d "/opt/gnollhack/server.gnollhack.com/$1/" ]; then
    echo "Directory /opt/gnollhack/server.gnollhack.com/$1 already exists."
else
  echo "Creating /opt/gnollhack/server.gnollhack.com/$1 and giving permissions."
  sudo mkdir /opt/gnollhack/server.gnollhack.com/$1
  sudo chmod a+rwx /opt/gnollhack/server.gnollhack.com/$1
fi

echo "Copying default rc files."
sudo cp ./dgl-default-rcfile /opt/gnollhack/server.gnollhack.com/$1/dgl-default-rcfile.gnh
sudo cp ./dgl-default-curses-putty-rcfile /opt/gnollhack/server.gnollhack.com/$1/dgl-default-curses-putty-rcfile.gnh
sudo cp ./dgl-default-curses-ssh-rcfile /opt/gnollhack/server.gnollhack.com/$1/dgl-default-curses-ssh-rcfile.gnh

echo "Deleting the old options files from users, if any."
sudo find /opt/gnollhack/server.gnollhack.com/dgldir/userdata -type f -name "*_$2.gnhrc" -delete

shopt -s nullglob dotglob
varfiles=(/opt/gnollhack/server.gnollhack.com/$1/var/*)
if  [ ${#varfiles[@]} -gt 0 ]; then
  echo "Deleting files from var directory."
  sudo find /opt/gnollhack/server.gnollhack.com/$1/var -maxdepth 1 -type f -delete
else
  echo "No files found in var directory."
fi

shopt -s nullglob dotglob
savefiles=(/opt/gnollhack/server.gnollhack.com/$1/var/save/*)
if  [ ${#savefiles[@]} -gt 0 ]; then
  echo "Deleting saved games."
  sudo rm /opt/gnollhack/server.gnollhack.com/$1/var/save/*
else
  echo "No saved files found."
fi

echo "Copying files to /opt/gnollhack/server.gnollhack.com/$1 and giving permissions"
sudo cp -R /$1/* /opt/gnollhack/server.gnollhack.com/$1
sudo chmod -R a+rwx /opt/gnollhack/server.gnollhack.com/$1/*

if [ -d "/opt/gnollhack/server.gnollhack.com/dgldir/inprogress-$1/" ]; then
    echo "Directory /opt/gnollhack/server.gnollhack.com/dgldir/inprogress-$1 already exists."
else
  echo "Creating /opt/gnollhack/server.gnollhack.com/dgldir/inprogress-$1 and giving permissions"
  sudo mkdir /opt/gnollhack/server.gnollhack.com/dgldir/inprogress-$1
  sudo chmod a+rwx /opt/gnollhack/server.gnollhack.com/dgldir/inprogress-$1
fi

echo "Finished"
