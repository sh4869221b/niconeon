import QtQuick
import QtTest
import Niconeon

TestCase {
    name: "Views"
    when: windowShown
    function test_mainAndDialogs() {
        const component = Qt.createComponent("qrc:/qt/qml/Niconeon/Main.qml")
        tryCompare(component, "status", Component.Ready)
        const window = component.createObject(null)
        verify(window !== null, component.errorString())
        wait(100)
        const settings = findChild(window, "settingsDialog")
        verify(settings !== null)
        settings.open()
        tryCompare(settings, "opened", true)
        settings.close()
        tryCompare(settings, "opened", false)
        const about = findChild(window, "aboutDialog")
        about.open()
        tryCompare(about, "opened", true)
        about.close()
        tryCompare(about, "opened", false)
        const filter = findChild(window, "filterDialog")
        filter.open()
        tryCompare(filter, "opened", true)
        filter.close()
        const player = findChild(window, "player")
        verify(player !== null)
        window.destroy()
    }
}
