using UnityEngine;

namespace Doctype.Samples
{
    /// <summary>
    /// A stand-in game to measure the HUD against: a grid of spinning cubes.
    /// </summary>
    /// <remarks>
    /// Measuring a UI over an idle scene answers the wrong question. A phone GPU
    /// with nothing to do finishes the HUD in the time it would have spent
    /// waiting, and the cost only appears once something else is already using
    /// the bandwidth and the tile memory. So the benchmark runs the HUD on top
    /// of a load rather than on top of a black screen, and reports what the HUD
    /// adds — not what it costs in isolation.
    /// <para>
    /// Deliberately dumb geometry with the default material: the point is a
    /// constant, reproducible load, not a realistic game. Anything adaptive
    /// (LOD, culling that depends on what the HUD covers) would let the load
    /// move underneath the measurement.
    /// </para>
    /// <para>
    /// The count has an upper bound that is easy to miss. At 240 cubes a
    /// 1080x2400 budget phone spent ~33 ms a frame on this alone, and every HUD
    /// scenario then fit inside the GPU's shadow and reported "+0.00 ms" —
    /// true, and useless. The load has to be heavy enough that the GPU is busy
    /// and light enough that the frame still has room for the thing being
    /// measured to show up in it.
    /// </para>
    /// </remarks>
    [AddComponentMenu("Doctype/Samples/Benchmark Load")]
    public class HtmlBenchmarkLoad : MonoBehaviour
    {
        [Tooltip("Draw calls, near enough: one cube is one.")]
        [SerializeField] private int _cubes = 72;

        [Tooltip("Degrees per second. Rotation exists so nothing can be cached between frames.")]
        [SerializeField] private float _spin = 40f;

        Transform[] _cubes3d;
        Camera _camera;

        /// <summary>How many objects the load is drawing, for the report header.</summary>
        public int Cubes => _cubes;

        void Awake()
        {
            _camera = GetComponentInChildren<Camera>();
            Build();
        }

        void Build()
        {
            _cubes3d = new Transform[_cubes];

            // A slab of cubes filling the view, spread in depth so they overdraw
            // each other the way scene geometry does.
            int perRow = Mathf.CeilToInt(Mathf.Sqrt(_cubes));

            var shared = new Material(Shader.Find("Universal Render Pipeline/Lit") ??
                                      Shader.Find("Standard") ??
                                      Shader.Find("Unlit/Color"))
            {
                hideFlags = HideFlags.HideAndDontSave,
            };

            for (int i = 0; i < _cubes; i++)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                go.name = "Load" + i;
                go.transform.SetParent(transform, false);

                // Colliders would cost physics time that has nothing to do with
                // what is being measured.
                Destroy(go.GetComponent<Collider>());
                go.GetComponent<MeshRenderer>().sharedMaterial = shared;

                int row = i / perRow;
                int col = i % perRow;

                go.transform.localPosition = new Vector3((col - perRow * 0.5f) * 1.4f,
                                                         (row - perRow * 0.5f) * 1.4f,
                                                         6f + (i % 7) * 1.1f);

                _cubes3d[i] = go.transform;
            }

            if (_camera != null)
            {
                _camera.transform.localPosition = new Vector3(0f, 0f, -perRow * 1.1f);
            }
        }

        void Update()
        {
            // One shared angle rather than per-cube state: the load has to be
            // identical on every run, and Random would make it a different
            // benchmark each time.
            float angle = Time.time * _spin;
            var rotation = Quaternion.Euler(angle, angle * 0.7f, 0f);

            for (int i = 0; i < _cubes3d.Length; i++)
            {
                _cubes3d[i].localRotation = rotation;
            }
        }
    }
}
