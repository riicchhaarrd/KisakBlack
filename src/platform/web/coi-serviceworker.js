/*
 * coi-serviceworker v0.1.7
 * Copyright (c) 2021 Guido Zuidhof
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
let coepCredentialless = false;

if (typeof window === "undefined") {
  self.addEventListener("install", () => self.skipWaiting());
  self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));

  self.addEventListener("message", (event) => {
    if (!event.data) {
      return;
    }
    if (event.data.type === "deregister") {
      self.registration.unregister()
        .then(() => self.clients.matchAll())
        .then((clients) => clients.forEach((client) => client.navigate(client.url)));
    } else if (event.data.type === "coepCredentialless") {
      coepCredentialless = event.data.value;
    }
  });

  self.addEventListener("fetch", (event) => {
    const originalRequest = event.request;
    if (originalRequest.cache === "only-if-cached" && originalRequest.mode !== "same-origin") {
      return;
    }

    const request = coepCredentialless && originalRequest.mode === "no-cors"
      ? new Request(originalRequest, { credentials: "omit" })
      : originalRequest;

    event.respondWith(fetch(request).then((response) => {
      if (response.status === 0) {
        return response;
      }

      const headers = new Headers(response.headers);
      headers.set("Cross-Origin-Embedder-Policy",
        coepCredentialless ? "credentialless" : "require-corp");
      if (!coepCredentialless) {
        headers.set("Cross-Origin-Resource-Policy", "cross-origin");
      }
      headers.set("Cross-Origin-Opener-Policy", "same-origin");

      return new Response(response.body, {
        status: response.status,
        statusText: response.statusText,
        headers,
      });
    }).catch((error) => console.error(error)));
  });
} else {
  (() => {
    const reloadedBySelf = window.sessionStorage.getItem("coiReloadedBySelf");
    window.sessionStorage.removeItem("coiReloadedBySelf");
    const coepDegrading = reloadedBySelf === "coepdegrade";
    const coi = {
      shouldRegister: () => !reloadedBySelf,
      shouldDeregister: () => false,
      coepCredentialless: () => true,
      coepDegrade: () => true,
      doReload: () => window.location.reload(),
      quiet: false,
      ...window.coi,
    };

    const navigatorRef = navigator;
    const controlling = navigatorRef.serviceWorker && navigatorRef.serviceWorker.controller;
    if (controlling && !window.crossOriginIsolated) {
      window.sessionStorage.setItem("coiCoepHasFailed", "true");
    }
    const coepHasFailed = window.sessionStorage.getItem("coiCoepHasFailed");

    if (controlling) {
      const reloadToDegrade = coi.coepDegrade()
        && !(coepDegrading || window.crossOriginIsolated);
      navigatorRef.serviceWorker.controller.postMessage({
        type: "coepCredentialless",
        value: (reloadToDegrade || (coepHasFailed && coi.coepDegrade()))
          ? false
          : coi.coepCredentialless(),
      });
      if (reloadToDegrade) {
        if (!coi.quiet) console.log("Reloading page to degrade COEP.");
        window.sessionStorage.setItem("coiReloadedBySelf", "coepdegrade");
        coi.doReload("coepdegrade");
      }
      if (coi.shouldDeregister()) {
        navigatorRef.serviceWorker.controller.postMessage({ type: "deregister" });
      }
    }

    if (window.crossOriginIsolated !== false || !coi.shouldRegister()) return;
    if (!window.isSecureContext) {
      if (!coi.quiet) console.log("COOP/COEP Service Worker requires a secure context.");
      return;
    }
    if (!navigatorRef.serviceWorker) {
      if (!coi.quiet) console.error("COOP/COEP Service Worker is unavailable.");
      return;
    }

    navigatorRef.serviceWorker.register(window.document.currentScript.src).then(
      (registration) => {
        if (!coi.quiet) console.log("COOP/COEP Service Worker registered", registration.scope);
        registration.addEventListener("updatefound", () => {
          if (!coi.quiet) console.log("Reloading page for the updated COOP/COEP Service Worker.");
          window.sessionStorage.setItem("coiReloadedBySelf", "updatefound");
          coi.doReload();
        });
        if (registration.active && !navigatorRef.serviceWorker.controller) {
          if (!coi.quiet) console.log("Reloading page for COOP/COEP isolation.");
          window.sessionStorage.setItem("coiReloadedBySelf", "notcontrolling");
          coi.doReload();
        }
      },
      (error) => console.error("COOP/COEP Service Worker registration failed:", error),
    );
  })();
}
