# File System Component

[![Badge](https://components.espressif.com/components/espp/file_system/badge.svg)](https://components.espressif.com/components/espp/file_system)

The `FileSystem` class provides a simple interface for interacting with the
filesystem. It provides a singleton class which manages the filesystem and also
includes the relevant POSIX, newlib, and C++ standard library headers providing
access to the filesystem. It is a wrapper around the `LittleFS` library and
can be configured using menuconfig to use a custom partition label.

It also provides some utility functions for interacting with the filesystem
and performing operations such as getting the total, used, and free space on
the filesystem, and listing files in a directory.

## Example

The [example](./example) shows how the `file_system` component can be used to
manage files on a [littlefs](https://github.com/littlefs-project/littlefs) file
system (wrapping the associated [esp_littlefs
component](https://github.com/joltwallet/esp_littlefs)).
