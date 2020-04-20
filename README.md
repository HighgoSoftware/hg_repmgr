hg_repmgr: Replication Manager for HighGo Database
==========================================

`hg_repmgr` is a suite of open-source tools to manage replication and failover
within a cluster of HighGo Database servers. It enhances HighGo Database's built-in
replication capabilities with utilities to set up standby servers, monitor
replication, and perform administrative tasks such as failover or switchover
operations.

`hg_repmgr` is a complete rewrite of the existing `repmgr` codebase, allowing
the use of all of the latest features in HighGo Database replication.

`hg_repmgr` is distributed under the GNU GPL 3 and maintained by 2ndQuadrant.

### BDR support

`hg_repmgr` supports monitoring of a two-node BDR 2.0 cluster on HighGo Database 4.5
only. Note that BDR 2.0 is not publicly available; please contact 2ndQuadrant
for details.

### Highgo changing
HighGo Software Co.,Ltd. made changes against repmgr, to support some special features

Documentation
-------------

The main `hg_repmgr` documentation is available here:

> [hg_repmgr documentation](https://repmgr.org/docs/4.2/index.html)


Files
------

 - `CONTRIBUTING.md`: details on how to contribute to `repmgr`
 - `COPYRIGHT`: Copyright information
 - `HISTORY`: Summary of changes in each `repmgr` release
 - `LICENSE`: GNU GPL3 details


Directories
-----------

 - `contrib/`: additional utilities
 - `doc/`: DocBook-based documentation files
 - `expected/`: expected regression test output
 - `scripts/`: example scripts
 - `sql/`: regression test input


Support and Assistance
----------------------

2ndQuadrant provides 24x7 production support for `repmgr`, including
configuration assistance, installation verification and training for
running a robust replication cluster. For further details see:

* https://2ndquadrant.com/en/support/

There is a mailing list/forum to discuss contributions or issues:

* https://groups.google.com/group/repmgr

The IRC channel #repmgr is registered with freenode.

Please report bugs and other issues to:

* https://github.com/2ndQuadrant/repmgr

Further information is available at https://www.repmgr.org/

We'd love to hear from you about how you use repmgr. Case studies and
news are always welcome. Send us an email at info@2ndQuadrant.com, or
send a postcard to

    repmgr
    c/o 2ndQuadrant
    7200 The Quorum
    Oxford Business Park North
    Oxford
    OX4 2JZ
    United Kingdom

Thanks from the repmgr core team.

* Ian Barwick
* Jaime Casanova
* Abhijit Menon-Sen
* Simon Riggs
* Cedric Villemain

Further reading
---------------

* https://blog.2ndquadrant.com/repmgr-3-2-is-here-barman-support-brand-new-high-availability-features/
* https://blog.2ndquadrant.com/improvements-in-repmgr-3-1-4/
* https://blog.2ndquadrant.com/managing-useful-clusters-repmgr/
* https://blog.2ndquadrant.com/easier_postgresql_90_clusters/
