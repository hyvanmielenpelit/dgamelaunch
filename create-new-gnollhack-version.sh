#!/bin/sh

if [ $# -eq 0 ]; then
  echo "No arguments. The first argument must be the name of the GnollHack directory. For example, ghn5."
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

if [ $# -eq 2 ]; then
  echo "Modifying the server address in dgamelaunch.conf to $2-server.gnollhack.com"
  sudo sed -i -r 's/\$ATTR\(14\)server.gnollhack.com/\$ATTR\(14\)$2-server.gnollhack.com/g' /opt/gnollhack/server.gnollhack.com/etc/dgamelaunch.conf
else
  echo "The server address in dgamelaunch.conf was not modified, since the second argument was not given."
  echo "If you want to change the server address automatically, the second argument needs to be, for example, 'eu'."
fi

echo "Copying default rc files"
sudo cp ./dgl-default-rcfile /opt/gnollhack/server.gnollhack.com/dgl-default-rcfile.gnh
sudo cp ./dgl-default-curses-putty-rcfile /opt/gnollhack/server.gnollhack.com/dgl-default-curses-putty-rcfile.gnh
sudo cp ./dgl-default-curses-ssh-rcfile /opt/gnollhack/server.gnollhack.com/dgl-default-curses-ssh-rcfile.gnh

if [ -d "/opt/gnollhack/server.gnollhack.com/$1/" ]; then
    echo "Directory /opt/gnollhack/server.gnollhack.com/$1 already exists."
else
  echo "Creating /opt/gnollhack/server.gnollhack.com/$1 and giving permissions"
  sudo mkdir /opt/gnollhack/server.gnollhack.com/$1
  sudo chmod a+rwx /opt/gnollhack/server.gnollhack.com/$1
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
