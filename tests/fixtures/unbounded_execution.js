(function () {
  function busyFor(milliseconds) {
    var end = performance.now() + milliseconds;
    while (performance.now() < end) {}
  }

  function incomingPort(delay) {
    return { send: function () { busyFor(delay); } };
  }

  function outgoingPort() {
    return { subscribe: function () {} };
  }

  globalThis.Elm = {
    Main: {
      init: function (options) {
        setTimeout(function () { busyFor(350); }, 0);
        Promise.resolve().then(function () { busyFor(350); });
        options.node.addEventListener("mouseup", function () {
          busyFor(5250);
        });

        return {
          ports: {
            execREGLCmd: outgoingPort(),
            setView: outgoingPort(),
            reglupdate: incomingPort(350),
            recvREGLCmd: incomingPort(350)
          }
        };
      }
    }
  };
})();
