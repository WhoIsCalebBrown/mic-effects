import QtQuick 2.15
import QtTest 1.3
import "../plugin" as Plugin

TestCase {
  name: "PlainText"

  Component {
    id: labelComponent
    Plugin.SafeText { }
  }

  function test_untrustedMarkupIsLiteralText() {
    const malicious = '<img src="https://attacker.invalid/pixel">'
    const label = createTemporaryObject(labelComponent, this, { text: malicious })

    verify(label)
    compare(label.textFormat, Text.PlainText)
    compare(label.text, malicious)
  }
}
