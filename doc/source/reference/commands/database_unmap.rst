.. -*- rst -*-

.. highlightlang:: none

.. groonga-command
.. database: commands_database_unmap

.. groonga-command
.. thread_count 2

``database_unmap``
==================

Summary
-------

.. versionadded:: 5.0.7

``database_unmap`` unmaps already mapped tables and columns in the
database. It releases memory to map tables and columns.

.. note::

   Normally, you don't need to use ``database_unmap`` because OS
   manages memory cleverly. If remained system memory is reduced, OS
   moves memory used by Groonga to disk until Groonga needs the
   memory. OS moves unused memory preferentially.

.. caution::

   You can use this command only when :doc:`thread_count` returns
   ``1``. It means that this command doesn't work with multithreading.

Syntax
------

``database_unmap`` has no parameters::

  database_unmap

Usage
-----

You can unmap database after you change the max number of threads to
``1``:

.. groonga-command
.. include:: ../../example/reference/commands/database_unmap/usage_success.log
.. thread_count --new_count 1
.. database_unmap

If the max number of threads is larger than ``1``, ``database_unmap``
fails:

.. groonga-command
.. include:: ../../example/reference/commands/database_unmap/usage_failure.log
.. thread_count --new_count 2
.. database_unmap

Parameters
----------

This section describes all parameters.

Required parameters
^^^^^^^^^^^^^^^^^^^

There is no required parameters.

Optional parameters
^^^^^^^^^^^^^^^^^^^

There is no optional parameters.

Return value
------------

The command returns ``true`` as body on success such as::

  [HEADER, true]

If the command fails, error details are in ``HEADER``.

See :doc:`/reference/command/output_format` for ``HEADER``.
