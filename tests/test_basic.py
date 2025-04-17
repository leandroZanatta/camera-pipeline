import unittest

class TestBasic(unittest.TestCase):
    def test_import(self):
        """Testa se o módulo core pode ser importado."""
        try:
            import core
            self.assertTrue(True)
        except ImportError:
            self.fail("Falha ao importar o módulo core")
    
    def test_processor_import(self):
        """Testa se o módulo processor pode ser importado."""
        try:
            from core import processor
            self.assertTrue(True)
        except ImportError:
            self.fail("Falha ao importar o módulo processor")

if __name__ == "__main__":
    unittest.main() 