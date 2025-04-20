from setuptools import setup, Extension

setup(
    ext_modules=[
        # Declara a existência e o local da extensão C compilada.
        # O nome é o caminho do módulo Python onde o .so residirá.
        # NENHUMA fonte é listada aqui; CMake cuida disso.
        Extension("camera_pipeline.core.camera_pipeline_c", sources=[])
    ]
    # Não adicione outros metadados aqui; scikit-build os lerá do pyproject.toml
) 