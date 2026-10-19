# UI components

React 19 + Vite, custom components and plain CSS. No shared primitive library. Icon is the sole general UI primitive; page-specific ClusterGauge, CameraView, MapView, CanLog and VehiclePanel are local to App.jsx.

## Icon
Source: `dashboard/src/App.jsx`; props: name, size. Inline stroke icons.
```jsx
function Icon({ name, size = 16 }) {
  const paths = {
    bolt: (
      <path d="m13 2-9 12h7l-1 8 9-12h-7l1-8Z" />
    ),

    battery: (
      <>
        <rect x="2" y="7" width="17" height="10" rx="2.5" />
        <path d="M22 11v2" />
      </>
    ),

    wifi: (
      <>
        <path d="M5 12.5a11 11 0 0 1 14 0M8 16a6.5 6.5 0 0 1 8 0" />
        <circle cx="12" cy="19.5" r="1" />
      </>
    ),

    gauge: (
      <>
        <path d="M4.9 17a8 8 0 1 1 14.2 0" />
        <path d="m12 13 3-3" />
      </>
    ),

    temp: (
      <>
        <path d="M14 14.8V4a2 2 0 1 0-4 0v10.8a4 4 0 1 0 4 0Z" />
      </>
    ),

    road: (
      <>
        <path d="M4 20 8 4M20 20 16 4M12 5v3M12 11v3M12 17v3" />
      </>
    ),

    alert: (
      <>
        <path d="M12 3 2 20h20L12 3Z" />
        <path d="M12 10v4M12 17.5v.5" />
      </>
    ),

    camera: (
      <>
        <path d="M3 8.5A2.5 2.5 0 0 1 5.5 6h1.8l1.2-2h6.6l1.2 2h1.8A2.5 2.5 0 0 1 20.5 8.5v8A2.5 2.5 0 0 1 18 19H5.5A2.5 2.5 0 0 1 3 16.5Z" />
        <circle cx="11.75" cy="12" r="3.4" />
      </>
    ),

    map: (
      <>
        <path d="M9 4 3 6.5v13L9 17l6 2.5 6-2.5v-13L15 6.5Z" />
        <path d="M9 4v13M15 6.5v13" />
      </>
    ),
  }

  return (
    <svg
      className="icon"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {paths[name]}
    </svg>
  )
}


```
