#!/bin/bash

function do_help()
{
    echo -e "Usage:
       install-script.sh -d <directory of PG Home> -u <the user to startup PG>

       For example, PG is installed in /opt/postgres11.5 and should be startup with user postgres
       The option -d is mandatory
       Please run the script as:

       ./install-script.sh -d /opt/postgres11.5 -u postgres\n" 
}


PGdir=""

while getopts "d:h" arg 
do
    case $arg in
    d)
        PGdir=$OPTARG
        echo $PGdir
        ;;
    h)
        do_help
        ;;
    ?)
        echo "unkonw argument"
        exit 1
        ;;
    esac
done

if [ "$PGdir" = "" ]
then
    do_help
    exit 1
fi

if [ "$(id -u)" != "0" ]
then
    echo "Error: please use root user to run this cmd"
    exit 1
fi

cp ./repmgr $PGdir/bin
cp ./repmgrd $PGdir/bin
cp ./repmgr.so $PGdir/lib
chmod 755 $PGdir/bin/repmgr
chmod 755 $PGdir/bin/repmgrd
chmod 755 $PGdir/lib/repmgr.so
chmod 644 $PGdir/conf/hg_repmgr.conf
cp ./repmgr*.sql $PGdir/share/extension/
cp ./repmgr.control $PGdir/share/extension/
chmod 644 $PGdir/share/extension/repmgr.control
chmod 644 $PGdir/share/extension/repmgr*.sql
mkdir $PGdir/conf
touch $PGdir/conf/hg_repmgr.conf

