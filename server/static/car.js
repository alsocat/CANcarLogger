// Which car the dashboard shows. Every /api/ request gets ?car=<VIN> added;
// with one car the picker stays hidden and nothing changes.
(function () {
  let car = "";
  try { car = new URLSearchParams(location.search).get("car") || localStorage.getItem("car") || ""; } catch (e) {}
  const realFetch = window.fetch.bind(window);
  window.fetch = (path, opts) => {
    if (car && typeof path === "string" && path.startsWith("/api/") && !/[?&]car=/.test(path)) {
      path += (path.includes("?") ? "&" : "?") + "car=" + encodeURIComponent(car);
    }
    return realFetch(path, opts);
  };
  window.carPicker = async (el) => {
    const list = await realFetch("/api/cars").then(r => r.json()).catch(() => []);
    if (!el || list.length < 2) return;
    el.innerHTML = list.map(c => `<option value="${c.car}">${c.name.replace(/</g, "&lt;")}</option>`).join("");
    el.value = car || list[0].car;
    el.hidden = false;
    el.addEventListener("change", () => {
      try { localStorage.setItem("car", el.value); } catch (e) {}
      location.reload();
    });
  };
})();
