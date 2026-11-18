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

    function test_ngToastAppearsAbovePlayerAndUndoWorks() {
        const component = Qt.createComponent("qrc:/qt/qml/Niconeon/Main.qml")
        tryCompare(component, "status", Component.Ready)
        const window = createTemporaryObject(component, null)
        verify(window !== null, component.errorString())
        const player = findChild(window, "player")
        const toast = findChild(window, "toast")
        verify(player !== null)
        verify(toast !== null)
        const undo = findChild(toast, "toastActionButton")
        verify(undo !== null)
        tryVerify(() => player.height > 0)
        verify(waitForRendering(player))

        application.danmaku.ngDropRequested("toast-test-user")
        tryCompare(toast, "actionText", "Undo")
        tryCompare(toast, "visible", true)
        tryVerify(() => application.ngUsers.indexOf("toast-test-user") >= 0)
        const sample = toast.mapToItem(player, 24, 8)
        verify(sample.x > 0 && sample.x < player.width && sample.y > 0 && sample.y < player.height)
        const point = player.mapToItem(window.contentItem, sample.x, sample.y)
        const pixel = grabImage(window.contentItem).pixel(Math.round(point.x), Math.round(point.y))
        verify(pixel.r > 0, "The toast background should be visible over the black player")
        mouseClick(undo)
        tryVerify(() => application.ngUsers.indexOf("toast-test-user") === -1)
        tryCompare(toast, "message", "NG登録を取り消しました")
    }
}
