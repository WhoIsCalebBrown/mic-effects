pragma Singleton
import QtQuick

QtObject {
  property bool socketAvailable: true

  function env(name) {
    return name === "HOME" ? "/test/home" : "/test/runtime"
  }
}
